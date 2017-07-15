#pragma once

#include "optional.h"
#include "fs_tree.h"
#include "build_rules.h"

#include <vector>
#include <string>
#include <functional>
#include <thread>

class Job {
    BuildRule m_rule;
    std::function<void(std::string,
                       std::function<void(void)>)> m_resolve_input_cb;
    std::function<void(void)> m_completion_cb;

    void th_execute(void);

    std::thread m_exec_thread;

public:
    explicit Job(const BuildRule &rule,
                 std::function<void(std::string,
                                    std::function<void(void)>)> resolve_input_cb,
                 std::function<void(void)> completion_cb)
        : m_rule(rule)
        , m_resolve_input_cb(resolve_input_cb)
        , m_completion_cb(completion_cb)
        , m_exec_thread([&]() {
                this->th_execute();
                this->m_completion_cb();
            })
    {
    };

    void wait();

    void want(std::string);
};
