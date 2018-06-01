#include "ErrorQueue.h"

namespace ruby_typer {
namespace core {
std::vector<std::unique_ptr<BasicError>> ErrorQueue::drainErrors() {
    ENFORCE(owner == std::this_thread::get_id());
    std::vector<std::unique_ptr<BasicError>> res;
    for (auto &alreadyCollected : collected) {
        for (auto &entry : alreadyCollected.second) {
            res.emplace_back(move(entry.error));
        }
    }
    collected.clear();
    ErrorQueueMessage msg;
    for (auto result = queue.try_pop(msg); result.gotItem(); result = queue.try_pop(msg)) {
        if (msg.kind == ErrorQueueMessage::Kind::Error) {
            res.emplace_back(std::move(msg.error));
        }
    }
    return res;
}

ErrorQueue::ErrorQueue(spd::logger &logger, spd::logger &tracer) : logger(logger), tracer(tracer) {
    owner = std::this_thread::get_id();
}

void ErrorQueue::renderForFile(core::FileRef whatFile, std::stringstream &critical, std::stringstream &nonCritical) {
    auto it = collected.find(whatFile);
    if (it == collected.end()) {
        return;
    }
    for (auto &error : it->second) {
        auto &out = error.error->isCritical ? critical : nonCritical;
        out << error.text;
    }
    collected[whatFile].clear();
};

void ErrorQueue::flushErrors(bool all) {
    ENFORCE(owner == std::this_thread::get_id());

    std::stringstream critical;
    std::stringstream nonCritical;

    ErrorQueueMessage msg;
    for (auto result = queue.try_pop(msg); result.gotItem(); result = queue.try_pop(msg)) {
        if (msg.kind == ErrorQueueMessage::Kind::Error) {
            this->errorCount.fetch_add(1);
            collected[msg.whatFile].emplace_back(std::move(msg));
        } else if (msg.kind == ErrorQueueMessage::Kind::Flush) {
            renderForFile(msg.whatFile, critical, nonCritical);
            renderForFile(core::FileRef(), critical, nonCritical);
        }
    }

    if (all) {
        for (auto &part : collected) {
            for (auto &error : part.second) {
                auto &out = error.error->isCritical ? critical : nonCritical;
                out << error.text;
            }
        }
        collected.clear();
    }

    if (critical.tellp() != 0) {
        this->logger.log(spdlog::level::critical, "{}", critical.str());
    }
    if (nonCritical.tellp() != 0) {
        this->logger.log(spdlog::level::err, "{}", nonCritical.str());
    }
}

ErrorQueue::~ErrorQueue() {
    flushErrors(true);
}
} // namespace core
} // namespace ruby_typer
