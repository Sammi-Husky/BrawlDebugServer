#pragma once

#include <memory.h>
#include <nt/network.h>
#include <types.h>

class OSThread;
class gfTask;
class gfTaskScheduler;
class Plugin;

#define DATA_CHUNK_SIZE 1024

struct TaskInfo {
    TaskInfo()
    {
        memset(this, 0, sizeof(TaskInfo));
    }
    char name[32];
    u32 id;
    char status;
    char category;
    bool alive;
    u32 address;
};

enum pause_state {
    PAUSE_UNPAUSED,
    PAUSE_PAUSED,
    PAUSE_STEP,
};

enum req_msg_t {
    MSG_REQ_INVALID = 0,
    MSG_REQ_TASKS = 1,
    MSG_REQ_TASK = 2,
    MSG_REQ_HEAPS = 3,
    MSG_REQ_DATA = 4,
    MSG_REQ_PAUSE = 5,
    MSG_REQ_UNPAUSE = 6,
    MSG_REQ_STEP = 7,
};

enum ctrl_msg_t {
    MSG_SEND_INVALID = 0,
    MSG_SEND_TASKS = 1,
    MSG_SEND_HEAPS = 2,
    MSG_SEND_RAW = 3,
};

/**
 * @brief Structure for request messages from the client.
 */
struct ReqMsg {
    req_msg_t type; // Request type
    u32 params[3];  // Optional parameters (e.g., task ID, length of data, etc.)
};

/**
 * @brief Structure for control messages sent to the client.
 */
struct CtrlMsg {
    // Control message
    ctrl_msg_t type;

    // Length of any payload that follows (if any)
    int length;
};

namespace DebugServer {
    void Init(Plugin* plg);
    void SendAllTasks();
    void SendTask(gfTask* task);
}