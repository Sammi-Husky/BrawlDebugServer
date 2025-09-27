#include "dbgsrv.h"
#include <hook.hpp>
#include <os/OSError.h>
#include <plugin.hpp>
namespace Sandbox {
    void Init(Plugin* plg)
    {
        DebugServer::Init(plg);
    }
}
