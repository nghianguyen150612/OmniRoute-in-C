/*
 * OmniRoute native backend — exit-code contract (Task 011).
 *
 * Three codes, no more: success, invalid CLI usage, and runtime failure.
 * Later tasks add behavior, not codes, unless a new failure class genuinely
 * needs distinguishing.
 */
#ifndef OMNIROUTE_EXIT_CODE_H
#define OMNIROUTE_EXIT_CODE_H

enum omni_exit_code {
  OMNI_EXIT_OK = 0, /* normal startup/shutdown completed */
  OMNI_EXIT_RUNTIME = 1, /* initialization or runtime failure */
  OMNI_EXIT_USAGE = 2 /* unknown option or unexpected positional argument */
};

#endif /* OMNIROUTE_EXIT_CODE_H */
