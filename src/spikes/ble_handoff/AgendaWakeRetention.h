#pragma once

#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

#include "AgendaWakePolicy.h"

namespace dashboard {

inline ReceiverResult retainedReceiverResult() { return decodeReceiverResultWord(REG_READ(RTC_CNTL_STORE0_REG)); }

inline void retainReceiverResult(const ReceiverResult result) {
  REG_WRITE(RTC_CNTL_STORE0_REG, encodeReceiverResultWord(result));
}

}  // namespace dashboard
