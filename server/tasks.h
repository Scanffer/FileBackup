#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace backup {

/// 异步任务的状态。
///
/// 状态只会沿 queued → running → succeeded/failed 单向前进：
///   queued    —— 已入队，等 worker 空闲
///   running   —— worker 正在执行（progress = 50）
///   succeeded —— 成功（progress = 100，result 非空）
///   failed    —— 失败（error 非空，progress 停在 50，表示没走完）
/// 后两个是终态：SSE 推完最后一个快照就关连接。
enum class TaskState {
    Queued,
    Running,
    Succeeded,
    Failed,
};

/// 写进 JSON 的稳定字符串（"queued" / "running" / "succeeded" / "failed"）。
/// 前端靠它判断任务是否结束，所以**不要**随手改名。
const char* to_string(TaskState state);

/// 任务成功时由任务体交回来的东西。
struct TaskOutcome {
    nlohmann::json result = nullptr;  ///< 原样写进任务 JSON 的 result 字段
    std::string message;              ///< 给用户看的一行结果说明
};

/// 任务体。**抛异常 = 任务失败**，异常消息即 error。
///
/// 它跑在 TaskManager 的 worker 线程里，所以可以放心做阻塞的文件操作；
/// 但异常消息绝对不能拼进密码（调用方负责，见 http_server.cpp）。
using TaskBody = std::function<TaskOutcome()>;

/// 任务的一次快照。纯值语义，可以安全地跨线程/跨 handler 传递。
struct TaskInfo {
    std::string id;                  ///< 形如 "task-1"
    std::string kind;                ///< "backup" / "restore"
    TaskState state = TaskState::Queued;
    int progress = 0;                ///< 0 / 50 / 100 粗粒度
    std::string message;             ///< 当前阶段的一句话（排队中/正在打包…/完成）
    std::string error;               ///< 仅失败时非空
    nlohmann::json result = nullptr; ///< 仅成功时非空
};

/// 单 worker 的异步任务管理器。
///
/// ---- 为什么是单 worker，而不是「每个任务一个线程 + 一把大锁」----
/// 备份/还原是重文件 I/O，两个任务同时写同一个目标目录会互相踩。用
/// 一个后台线程按提交顺序串行执行，天然满足「文件操作串行化」，业务
/// 代码里也不用再散落全局锁；「排队」还能反映到任务状态上给用户看。
///
/// ---- 线程安全 ----
/// 所有 public 方法都可以从任意线程调用；submit 只负责入队并立刻返回
/// 任务 id，真正的文件操作留到 worker 线程。
class TaskManager {
public:
    TaskManager();
    ~TaskManager();
    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;

    /// 入队一个任务，立刻返回任务 id（"task-1"、"task-2"……）。
    /// running_message 是任务开始执行时展示的一句阶段说明。
    std::string submit(std::string kind, std::string running_message,
                       TaskBody body);

    /// 取某个任务的快照；id 不存在（或已被历史清理）返回 nullopt。
    std::optional<TaskInfo> get(const std::string& id) const;

    /// 全部任务的快照，**最新在前**（任务面板按这个顺序渲染）。
    std::vector<TaskInfo> list() const;

    /// 任务快照 → API / SSE 使用的 JSON 对象。
    /// 字段固定为 id/kind/state/progress/message/error/result，
    /// 未发生的信息用 null，前端不用自己补字段。
    static nlohmann::json to_json(const TaskInfo& info);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace backup
