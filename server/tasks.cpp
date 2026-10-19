#include "tasks.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace backup {
namespace {

/// 内存里最多保留多少条任务记录。
///
/// 这是个本地单用户工具，一次演示最多几十个任务；留 200 条足够任务
/// 面板翻看，又不会让记录无限增长。超出后从最老的**已结束**任务开始
/// 清理（排队/运行中的永远不删）。
constexpr std::size_t kMaxTasks = 200;

}  // namespace

const char* to_string(TaskState state) {
    switch (state) {
        case TaskState::Queued: return "queued";
        case TaskState::Running: return "running";
        case TaskState::Succeeded: return "succeeded";
        case TaskState::Failed: return "failed";
    }
    return "failed";
}

struct TaskManager::Impl {
    /// 一个排队中的任务：任务体要等 worker 线程来领。
    struct Pending {
        std::string id;
        std::string running_message;
        TaskBody body;
    };

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool stopping = false;

    std::uint64_t next_id = 0;

    /// id -> 任务快照。worker 直接原地修改它。
    std::map<std::string, TaskInfo> tasks;
    /// 提交顺序（只增不重排），用于 list 的「最新在前」与历史清理。
    std::vector<std::string> order;
    /// 待执行队列；worker 每次取队首。
    std::deque<Pending> queue;

    Impl() { worker = std::thread([this] { run(); }); }
    ~Impl() { shutdown(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void shutdown() {
        std::deque<Pending> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            stopping = true;
            dropped.swap(queue);
            // 还没开始跑的排队任务直接判失败：服务都要关了，
            // 不能让它们以后台线程的身份在退出过程中再跑一遍。
            for (const Pending& pending : dropped) {
                auto it = tasks.find(pending.id);
                if (it == tasks.end()) continue;
                if (it->second.state != TaskState::Queued) continue;
                it->second.state = TaskState::Failed;
                it->second.message = "服务已停止";
                it->second.error = "服务已停止，排队中的任务被取消";
            }
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    /// 超出 kMaxTasks 时从最老的已结束任务开始清理。要求已持有 mutex。
    void prune_locked() {
        while (order.size() > kMaxTasks) {
            const std::string& oldest = order.front();
            auto it = tasks.find(oldest);
            if (it != tasks.end()) {
                // 排队/运行的不能删：SSE 还在盯着它，删了会变成「任务不存在」。
                if (it->second.state == TaskState::Queued ||
                    it->second.state == TaskState::Running) {
                    break;
                }
                tasks.erase(it);
            }
            order.erase(order.begin());
        }
    }

    void run() {
        for (;;) {
            Pending pending;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping) return;

                pending = std::move(queue.front());
                queue.pop_front();

                auto it = tasks.find(pending.id);
                if (it == tasks.end()) continue;  // 理论上不会发生
                it->second.state = TaskState::Running;
                it->second.progress = 50;
                it->second.message = pending.running_message;
            }

            // 任务体在锁外跑：文件操作可能很慢，不能占着状态锁。
            TaskOutcome outcome;
            std::string error;
            bool ok = true;
            try {
                if (pending.body) outcome = pending.body();
            } catch (const std::exception& e) {
                ok = false;
                error = e.what();
            } catch (...) {
                ok = false;
                error = "未知错误";
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = tasks.find(pending.id);
                if (it != tasks.end()) {
                    if (ok) {
                        it->second.state = TaskState::Succeeded;
                        it->second.progress = 100;
                        it->second.message =
                            outcome.message.empty() ? "完成" : outcome.message;
                        it->second.result = std::move(outcome.result);
                    } else {
                        it->second.state = TaskState::Failed;
                        // 停在运行阶段：50% 处失败，前端不会显示成「100% 完成」。
                        it->second.progress = 50;
                        it->second.message = "失败";
                        it->second.error =
                            error.empty() ? std::string("未知错误") : error;
                    }
                }
                prune_locked();
            }
            cv.notify_all();
        }
    }
};

TaskManager::TaskManager() : impl_(std::make_unique<Impl>()) {}

TaskManager::~TaskManager() = default;

std::string TaskManager::submit(std::string kind, std::string running_message,
                                TaskBody body) {
    if (!body) throw std::invalid_argument("任务体不能为空");

    std::string id;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);

        id = "task-" + std::to_string(++impl_->next_id);

        TaskInfo info;
        info.id = id;
        info.kind = std::move(kind);
        info.state = TaskState::Queued;
        info.progress = 0;
        info.message = "排队中";
        impl_->tasks.emplace(id, std::move(info));
        impl_->order.push_back(id);
        impl_->queue.push_back(Impl::Pending{
            id, std::move(running_message), std::move(body)});
        impl_->prune_locked();
    }
    impl_->cv.notify_one();  // 唤醒 worker 要在 unlock 之后，避免刚唤醒又被抢锁
    return id;
}

std::optional<TaskInfo> TaskManager::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->tasks.find(id);
    if (it == impl_->tasks.end()) return std::nullopt;
    return it->second;
}

std::vector<TaskInfo> TaskManager::list() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<TaskInfo> result;
    result.reserve(impl_->order.size());
    // 最新在前：任务面板通常把刚提交的放在最上面。
    for (auto it = impl_->order.rbegin(); it != impl_->order.rend(); ++it) {
        const auto found = impl_->tasks.find(*it);
        if (found != impl_->tasks.end()) result.push_back(found->second);
    }
    return result;
}

nlohmann::json TaskManager::to_json(const TaskInfo& info) {
    nlohmann::json body = nlohmann::json::object();
    body["id"] = info.id;
    body["kind"] = info.kind;
    body["state"] = to_string(info.state);
    body["progress"] = info.progress;
    body["message"] = info.message;
    body["error"] = info.error.empty() ? nlohmann::json(nullptr)
                                       : nlohmann::json(info.error);
    body["result"] = info.result.is_null() ? nlohmann::json(nullptr)
                                           : info.result;
    return body;
}

}  // namespace backup
