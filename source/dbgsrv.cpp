#include "dbgsrv.h"
#include <OS/OSThread.h>
#include <VI/vi.h>
#include <cstdio>
#include <gf/gf_task_scheduler.h>
#include <hook.hpp>
#include <plugin.hpp>

#define STACK_SIZE 0x4000

TaskInfo* getTaskInfo(gfTask* task)
{
    TaskInfo* info = new (Heaps::Network) TaskInfo();
    int nameLen = strlen(task->m_taskName);
    if (nameLen > sizeof(info->name) - 1)
        nameLen = sizeof(info->name) - 1;
    strncpy(info->name, task->m_taskName, nameLen);
    info->id = task->m_taskId;
    info->status = task->getStatus();
    info->category = task->m_taskCategory;
    info->alive = task->isAlive();
    info->address = (u32)task;
    return info;
}

namespace DebugServer {
    OSThread thread;
    int dataSock = -1;
    int ctrlSock = -1;
    char* stack;
    pause_state pauseState = PAUSE_UNPAUSED;

    /**
     * Creates a TCP server socket listening on the specified port.
     * @return The server socket descriptor, or a negative value on error.
     */
    static int create_server(int port, int maxClients)
    {
        // create addr
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = SOHtoNl(INADDR_ANY);
        addr.sin_port = SOHtoNs(port);

        int server = socket(AF_INET, SOCK_STREAM, 0);
        SetReceiveBufferSize(server, 0x4000);
        SetSendBufferSize(server, 0x4000);
        if (bind(server, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        {
            return -2;
        }
        if (listen(server, maxClients) < 0)
        {
            return -3;
        }

        OSReport("Server listening on port %d\n", port);

        return server;
    }

    /**
     * Disconnects the current client, if any.
     */
    void disconnect()
    {
        closesocket(dataSock);
        closesocket(ctrlSock);
        dataSock = -1;
        ctrlSock = -1;

        OSReport("Client disconnected\n");
    }

    /**
     * Sends a control message to the connected client.
     * @param code The control message code.
     * @param length The total length of the transfer to follow on the data socket, if any.
     */
    void SendControlMsg(int code, int length = 0)
    {
        if (CanSendOnSocket(ctrlSock))
        {
            char buffer[8];
            *(int*)&buffer[0] = code;
            *(int*)&buffer[4] = length;

            if (send(ctrlSock, buffer, sizeof(buffer), 0) < 0)
            {
                disconnect();
                return;
            }
        }
    }

    /**
     * Sets the paused state of all tasks.
     * @param paused True to pause all tasks, false to unpause.
     */
    void SetTasksPaused(bool paused)
    {
        *(((char*)g_taskScheduler) + 0xb) = paused;
        pauseState = paused ? PAUSE_PAUSED : PAUSE_UNPAUSED;
    }

    /**
     * Unpauses all tasks and sets the server to step mode.
     * In step mode, tasks will run for a single frame before pausing again.
     */
    void StepTasks()
    {
        SetTasksPaused(false);
        pauseState = PAUSE_STEP;
    }

    /**
     * Sends information about all registered tasks to the connected client.
     * This function gathers task information and sends it in a single bulk transfer.
     */
    void SendAllTasks()
    {
        char* buffer = new char[0x1000];

        // Hoisting the inner loop index to track
        // total tasks across multiple loops
        int x = 0;
        for (int i = 0; i < 17; i++)
        {
            // unk14 list is private so we have to access it via offset :(
            gfTask* task = ((gfTask**)g_taskScheduler + 0x14)[i];

            while (task != NULL)
            {
                TaskInfo* info = getTaskInfo(task);
                memcpy(&buffer[x * sizeof(TaskInfo)], info, sizeof(TaskInfo));
                task = task->m_prev;
                x++;
                delete info;
            }
        }

        if (CanSendOnSocket(dataSock))
        {
            // Send control message indicating the start of a bulk transfer
            int payloadSize = sizeof(TaskInfo) * x;
            SendControlMsg(MSG_SEND_TASKS, payloadSize);

            if (send(dataSock, buffer, payloadSize, 0) < 0)
            {
                disconnect();
                return;
            }
        }
        delete[] buffer;
    }

    /**
     * Sends information about a specific task to the connected client.
     * @param task The task to send information about.
     */
    void SendTask(gfTask* task)
    {
        TaskInfo* info = getTaskInfo(task);
        if (!info)
            return;

        if (CanSendOnSocket(dataSock))
        {
            // Send the payload
            if (send(dataSock, info, sizeof(TaskInfo), 0) < 0)
            {
                disconnect();
                return;
            }
        }

        delete info;
    }

    /**
     * Sends raw data from a specified memory address to the connected client.
     * @param msg The request message containing parameters for the data transfer.
     */
    void SendRawData(const ReqMsg& msg)
    {
        int length = msg.params[1];
        char* buffer = (char*)msg.params[0];

        if (length <= 0)
            return;

        if (CanSendOnSocket(dataSock))
        {
            // Send control message indicating the start of a bulk transfer
            SendControlMsg(MSG_SEND_RAW, length);

            if (send(dataSock, buffer, length, 0) < 0)
            {
                disconnect();
                return;
            }
        }
        delete[] buffer;
    }

    /**
     * Processes incoming requests from the connected client.
     * @param clientSock The client socket descriptor.
     */
    void processRequests()
    {
        if (ctrlSock < 0)
            return;

        if (CanReceiveOnSocket(ctrlSock))
        {
            ReqMsg msg;
            if (recv(ctrlSock, &msg, sizeof(msg), 0) < 0)
            {
                disconnect();
                return;
            }

            int command = msg.type;
            switch (command)
            {
            case MSG_REQ_TASKS:
                SendAllTasks();
                break;
            case MSG_REQ_PAUSE:
                SetTasksPaused(true);
                break;
            case MSG_REQ_UNPAUSE:
                SetTasksPaused(false);
                break;
            case MSG_REQ_STEP:
                StepTasks();
                break;
            case MSG_REQ_DATA:
                SendRawData(msg);
                break;
            default:
                break;
            }
        }
    }

    /**
     * Thread function to handle incoming connections and requests.
     *
     * @return Always returns NULL.
     */
    static void* process(void* param)
    {
        int ctrlServer = create_server(6969, 5);
        int dataServer = create_server(6970, 5);
        if (ctrlServer < 0 || dataServer < 0)
        {
            OSReport("Failed to create server\n");
            return NULL;
        }

        while (1)
        {
            if (dataSock <= 0 || ctrlSock <= 0)
            {
                struct sockaddr_in client;
                struct sockaddr_in ctrlClient;
                u32 len = sizeof(client);
                ctrlSock = accept(ctrlServer, (struct sockaddr*)&client, &len);
                dataSock = accept(dataServer, (struct sockaddr*)&ctrlClient, &len);
                if (dataSock < 0 || ctrlSock < 0)
                {
                    OSReport("Error accepting connection\n");
                    continue;
                }

                OSReport("Client connected: %s\n", SOInetNtoA(client.sin_addr));
            }

            // Process any requests from the connected client
            processRequests();

            VIWaitForRetrace();
        }

        return NULL;
    }

    /**
     * Hook function to implement step mode in the task scheduler.
     *
     * If the server is in step mode when this hook is reached, this function pauses all tasks.
     * This should be after exactly one frame of task processing.
     * @param scheduler Pointer to the task scheduler instance.
     */
    void StepModeHook()
    {
        if (pauseState == PAUSE_STEP)
        {
            SetTasksPaused(true);
            pauseState = PAUSE_PAUSED;
        }
    }

    void Init(Plugin* plg)
    {
        plg->addHookEx(0x8002e798, StepModeHook, SyringeCore::OPT_DIRECT);

        stack = new (Heaps::Network) char[STACK_SIZE];
        OSCreateThread(&thread, process, NULL, stack + STACK_SIZE, STACK_SIZE, 31, 0);
        OSResumeThread(&thread);
    }
}