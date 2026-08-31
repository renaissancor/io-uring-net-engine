#include "thread_role.h"

namespace app::detail {

thread_local thread_role tls_role = thread_role::none;

}  // namespace app::detail
