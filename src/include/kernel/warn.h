#ifndef WARN_H
#define WARN_H

#include "kernel/backtrace.h"
#include "serial.h"

/**
 * WARN_ON - Print warning with stack trace and continue.
 * @condition: Expression that should be false (warns if true).
 *
 * Unlike ASSERT, this does NOT halt the system.  The stack trace is
 * printed to serial, which is safe in any context including ISRs.
 *
 * NOTE: After Phase 2 adds vsnprintf, replace the chained log_serial
 * calls with a single formatted message.
 */
#define WARN_ON(condition) do {                                         \
    if ((condition)) {                                                   \
        log_serial("WARN_ON: " #condition " at ");                       \
        log_serial(__FILE__);                                            \
        log_serial(":");                                                 \
        /* __LINE__ as decimal — basic int-to-string */                 \
        int _warn_line_ = __LINE__;                                      \
        char _warn_buf_[12];                                             \
        int _warn_pos_ = 0;                                              \
        if (_warn_line_ == 0) _warn_buf_[_warn_pos_++] = '0';            \
        else {                                                           \
            char _warn_tmp_[8];                                          \
            int _warn_k_ = 0;                                            \
            while (_warn_line_ > 0) {                                    \
                _warn_tmp_[_warn_k_++] = '0' + (_warn_line_ % 10);       \
                _warn_line_ /= 10;                                       \
            }                                                            \
            while (_warn_k_ > 0)                                         \
                _warn_buf_[_warn_pos_++] = _warn_tmp_[--_warn_k_];       \
        }                                                                \
        _warn_buf_[_warn_pos_] = '\0';                                   \
        log_serial(_warn_buf_);                                          \
        log_serial("\n");                                                \
        dump_stack_serial();                                             \
    }                                                                    \
} while(0)

#endif /* WARN_H */
