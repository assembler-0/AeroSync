/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/udm.h
 * @brief Unified Driver Management - High-level orchestration API
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>

struct device;

/**
 * enum udm_state - System-wide driver state
 */
enum udm_state {
  UDM_STATE_RUNNING,
  UDM_STATE_SUSPENDING,
  UDM_STATE_SUSPENDED,
  UDM_STATE_RESUMING,
  UDM_STATE_SHUTTING_DOWN,
  UDM_STATE_HALTED,
};

/**
 * enum udm_driver_state - Per-driver state
 */
enum udm_driver_state {
  UDM_DRIVER_ACTIVE,
  UDM_DRIVER_SUSPENDED,
  UDM_DRIVER_STOPPED,
  UDM_DRIVER_ERROR,
};

/**
 * udm_suspend_all - Suspend all drivers in the system
 * @return 0 on success, negative error code on failure
 */
int udm_suspend_all(void);

/**
 * udm_resume_all - Resume all suspended drivers
 * @return 0 on success, negative error code on failure
 */
int udm_resume_all(void);

/**
 * udm_stop_all - Stop all drivers gracefully
 * @return 0 on success, negative error code on failure
 */
int udm_stop_all(void);

/**
 * udm_restart_all - Restart all stopped drivers
 * @return 0 on success, negative error code on failure
 */
int udm_restart_all(void);

/**
 * udm_shutdown_all - Shutdown all drivers for system power-off
 */
void udm_shutdown_all(void);

/**
 * udm_emergency_stop_all - Emergency stop all drivers (no error checking)
 */
void udm_emergency_stop_all(void);

/**
 * udm_get_state - Get current system-wide UDM state
 */
enum udm_state udm_get_state(void);

/**
 * udm_get_driver_state - Get state of a specific device
 */
enum udm_driver_state udm_get_driver_state(struct device *dev);

/**
 * udm_suspend_device - Suspend a specific device
 */
int udm_suspend_device(struct device *dev);

/**
 * udm_resume_device - Resume a specific device
 */
int udm_resume_device(struct device *dev);

/**
 * udm_init - Initialize UDM subsystem
 */
int udm_init(void);
