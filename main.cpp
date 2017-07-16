#include "assert.h"
#include "build_rules.h"
#include "job.h"

#include <cinttypes>
#include <vector>
#include <deque>
#include <string>
#include <map>
#include <set>
#include <thread>
#include <condition_variable>
#include <mutex>

class ResolveRequest {
public:
    const std::string target;
    const std::function<void(const Optional<BuildRule> &)> *const cb;

    explicit ResolveRequest(std::string t)
        : target(t), cb(nullptr) { }
    ResolveRequest(std::string t, const std::function<void(const Optional<BuildRule> &)> *f)
        : target(t), cb(f) { }
};

struct RunnerState {
    std::deque<ResolveRequest> resolve_queue;
    std::mutex resolve_mtx;
    std::condition_variable resolve_cv;

    std::map<BuildRule, Job*> active_jobs;
    std::deque<Job *> done_jobs;
    std::map<BuildRule, Outcome> outcomes;
    std::mutex mtx;

    void enqueue_resolve(std::string target,
                         std::function<void(const Optional<BuildRule> &)> *cb) {
        std::unique_lock<std::mutex> lck (this->resolve_mtx);
        this->resolve_queue.push_back(ResolveRequest(target, cb));
        this->resolve_cv.notify_all();
    }
    void wait_for_resolve_queue() {
        std::unique_lock<std::mutex> lck (this->resolve_mtx);
        this->resolve_cv.wait(lck);
    }
};

void resolve_all(BuildRules &build_rules,
                 RunnerState &runner_state,
                 const ResolveRequest &req,
                 std::map<std::string, Optional<BuildRule>> &rules_cache,
                 std::deque<BuildRule> &rules)
{
    {
        auto cached = rules_cache.find(req.target);
        if (cached != rules_cache.end()) {
            auto found_rule = cached->second;
            DEBUG("Invoking callback on: " << (found_rule.has_value() ? found_rule.get_value().to_string() : "<none>"));
            if (req.cb) (*req.cb)(found_rule);
            return;
        }
    }
    DEBUG("Resolving: " << req.target);
    const Optional<BuildRule> orule = build_rules.query(req.target);
    rules_cache[req.target] = orule;
    DEBUG("Done Resolving: " << req.target);
    if (orule.has_value()) {
        const BuildRule &rule = orule.get_value();
        for (auto input : rule.inputs) {
            runner_state.enqueue_resolve(input, nullptr);
        }
        rules.push_back(rule);
    }
    DEBUG("Invoking callback on: " << (orule.has_value() ? orule.get_value().to_string() : "<none>"));
    if (req.cb) (*req.cb)(orule);
}

static void done_handler(RunnerState *runner_state, std::function<void(void)> done,
                         const Optional<BuildRule> &rule);

static void run_job(const BuildRule &rule,
                    RunnerState &runner_state)
{
    // 1. execute command
    // 2. all forks/execs done by this command are allowed in parallel
    // 3. at most one resolution of a command's input is run in parallel,
    //    any more are put on the queue

    std::unique_lock<std::mutex> lck (runner_state.mtx);

    if (runner_state.active_jobs.find(rule) != runner_state.active_jobs.end()) {
        return;
    }
    if (runner_state.outcomes.find(rule) != runner_state.outcomes.end()) {
        return;
    }

    DEBUG("Running: " << rule.to_string());

    auto resolve_cb = [rule, &runner_state](std::string input, std::function<void(void)> done) {
        DEBUG("resolve cb: " << input);
        std::unique_lock<std::mutex> lck (runner_state.mtx);
        auto f = new std::function<void(const Optional<BuildRule> &)>(
            std::bind(&done_handler, &runner_state, done,  std::placeholders::_1));
        runner_state.resolve_queue.push_back(ResolveRequest(input, f));
    };

    auto completion_cb = [rule, &runner_state](void) {
        DEBUG("Done: '" << rule.to_string() << "'");
        std::unique_lock<std::mutex> lck (runner_state.mtx);
        auto found_job = runner_state.active_jobs.find(rule);
        ASSERT(found_job != runner_state.active_jobs.end());
        runner_state.done_jobs.push_back(found_job->second);
        auto erased_count = runner_state.active_jobs.erase(rule);
        ASSERT(1 == erased_count);
        DEBUG("Done job: " << found_job->second);
        runner_state.outcomes[rule] = Outcome();
    };

    Job *const job = new Job(rule, resolve_cb, completion_cb);
    runner_state.active_jobs[rule] = job;
    DEBUG("Added " << rule.to_string() << " with job " << job);
    lck.unlock();

    job->execute();
    DEBUG("Done " << rule.to_string() << " with job " << job);
}


static void done_handler(RunnerState *runner_state, std::function<void(void)> done,
                         const Optional<BuildRule> &rule)
{
    // DEBUG("done resolve cb: " << input);
    std::unique_lock<std::mutex> lck (runner_state->mtx);
    const bool not_build_yet = (rule.has_value()
                                && (runner_state->outcomes.find(rule.get_value()) == runner_state->outcomes.end()));
    lck.unlock();
    if (not_build_yet) {
        run_job(rule.get_value(), *runner_state);
    }
    done();
}

constexpr const uint32_t max_concurrent_jobs = 4;

void build(BuildRules &build_rules, const std::vector<std::string> &targets)
{
    RunnerState runner_state;
    for (auto target : targets) {
        DEBUG("Enqueing: " << target);
        runner_state.resolve_queue.push_back(ResolveRequest(target));
    }
    std::map<std::string, Optional<BuildRule>> rules_cache;
    std::deque<BuildRule> job_queue;

    bool shutdown = false;
    const uint32_t runners_count = 4;
    struct ThreadInfo {
        std::thread *thread;
        Optional<BuildRule> o_rule;
        std::mutex mutex;
        std::condition_variable cv;
    };
    ThreadInfo runners[runners_count];
    for (auto &th : runners) {
        th.o_rule = Optional<BuildRule>();
        th.thread = new std::thread([&th, &shutdown, &runner_state, &job_queue]() {
                while (!shutdown) {
                    std::unique_lock<std::mutex> lck(th.mutex);
                    while (!th.o_rule.has_value()) {
                        th.cv.wait(lck);
                    }
                    run_job(th.o_rule.get_value(), runner_state);
                    th.o_rule = Optional<BuildRule>();
                }
            });
    }

    std::thread resolve_th([&build_rules, &shutdown, &runner_state, &rules_cache]() {
            while (!shutdown) {
                runner_state.wait_for_resolve_queue();
                ASSERT(runner_state.resolve_queue.size() > 0);
                while (runner_state.resolve_queue.size() > 0) {
                    auto req = runner_state.resolve_queue.front();
                    runner_state.resolve_queue.pop_front();
                    resolve_all(build_rules, req, runner_state, rules_cache, job_queue);
                }
            }
        });

    while (true)
    {
        // TODO: bg thread? or use async IO and a reactor?
        std::unique_lock<std::mutex> lck (runner_state.mtx);

        if ((job_queue.size() > 0)
            && (runner_state.active_jobs.size() < max_concurrent_jobs))
        {
            auto rule = job_queue.front();
            job_queue.pop_front();
            for (auto &th : runners) {
                std::unique_lock<std::mutex> lck(th.mutex);
                if (th.o_rule.has_value()) continue;
                th.o_rule = Optional<BuildRule>(rule);
                th.cv.notify_all();
            }
        }

        if (runner_state.resolve_queue.size() > 0
            || (job_queue.size() > 0)
            || (runner_state.done_jobs.size() > 0)
            || (runner_state.active_jobs.size() > 0))
        {
            lck.unlock();

        }
        while (runner_state.done_jobs.size() > 0) {
            auto job = runner_state.done_jobs.front();
            runner_state.done_jobs.pop_front();
            delete job;
        }
        std::this_thread::sleep_for(100 * std::chrono::milliseconds());
    }

    shutdown = true;
    resolve_th.join();
    for (auto &th : runners) {
        th.thread->join();
        delete th.thread;
        th.thread = nullptr;
    }
}

int main(int argc, char **argv)
{
    ASSERT(argc >= 0);
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <query program> <target>" << std::endl;
        return 1;
    }

    DEBUG("Main: " << argc);

    BuildRules build_rules(argv[1]);

    std::vector<std::string> targets;
    for (uint32_t i = 2; i < (uint32_t)argc; i++) {
        targets.emplace_back(argv[i]);
    }

    build(build_rules, targets);

    return 0;
}
