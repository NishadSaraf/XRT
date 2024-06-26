// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2018-2022 Xilinx, Inc
// Copyright (C) 2022-2024 Advanced Micro Devices, Inc. All rights reserved.

#include <sys/stat.h>
#include <sys/types.h>

#include "app/xmaerror.h"
#include "app/xmalogger.h"
#include "app/xmaparam.h"
#include "lib/xmaapi.h"
#include "lib/xmalimits_lib.h"
//#include "lib/xmahw_hal.h"
#include "lib/xmalogger.h"
#include "app/xma_utils.hpp"
#include "lib/xma_utils.hpp"
#include <iostream>
#include <thread>
#include <algorithm>
#include "core/common/config_reader.h"
#include "core/common/device.h"
#include "core/common/message.h"
#include "core/common/system.h"

#define XMAAPI_MOD "xmaapi"

//Create singleton on the stack
static XmaSingleton xma_singleton_internal;

XmaSingleton *g_xma_singleton = &xma_singleton_internal;

int32_t xma_get_default_ddr_index(int32_t dev_index, int32_t cu_index, char* cu_name) {
    if (!g_xma_singleton->xma_initialized) {
        xma_logmsg(XMA_ERROR_LOG, XMAAPI_MOD,
                   "ddr_index can be obtained only after xma_initialization\n");
        return -1;
    }
/*
    bool expected = false;
    bool desired = true;
    while (!(g_xma_singleton->locked).compare_exchange_weak(expected, desired)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        expected = false;
    }
*/
    std::lock_guard<std::mutex> guard1(g_xma_singleton->m_mutex);
    //Singleton lock acquired

    if (cu_index < 0) {
        cu_index = xma_core::utils::get_cu_index(dev_index, cu_name);
        if (cu_index < 0) {
            //Release singleton lock
            //g_xma_singleton->locked = false;
            return -1;
        }
    }
    int32_t ddr_index = xma_core::utils::get_default_ddr_index(dev_index, cu_index);
    //Release singleton lock
    //g_xma_singleton->locked = false;

    return ddr_index;
}

void xma_thread1() {
    std::promise<bool> p;
    g_xma_singleton->thread1_future = p.get_future();
    p.set_value_at_thread_exit(true);

    bool expected = false;
    bool desired = true;
    std::list<XmaLogMsg> list1;
    while (!g_xma_singleton->xma_exit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        while (!g_xma_singleton->log_msg_list_locked.compare_exchange_weak(expected, desired)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            expected = false;
        }
        //log msg list lock acquired

        if (!g_xma_singleton->log_msg_list.empty()) {
            auto itr1 = list1.end();
            list1.splice(itr1, g_xma_singleton->log_msg_list);
        }

        //Release log msg list lock
        g_xma_singleton->log_msg_list_locked = false;

        while (!list1.empty()) {
            auto itr1 = list1.begin();
            xrt_core::message::send(static_cast<xrt_core::message::severity_level>(itr1->level),
                                    "XMA", itr1->msg.c_str());
            list1.pop_front();
        }

        if (!g_xma_singleton->xma_exit) {
            //Check Session loading
            uint32_t num_cmds = 0;
            //bool expected = false;
            //bool desired = true;
            XmaHwSessionPrivate *slowest_session = nullptr;
            uint32_t session_cmd_busiest_val = 0;
            std::lock_guard lock(g_xma_singleton->m_mutex);

            for (auto& itr1: g_xma_singleton->all_sessions_vec) {
                if (g_xma_singleton->xma_exit) {
                    break;
                }
                XmaHwSessionPrivate *priv1 = (XmaHwSessionPrivate*) itr1.hw_session.private_do_not_use;
                if (priv1 == NULL) {
                    xma_logmsg(XMA_ERROR_LOG, XMAAPI_MOD, "XMA thread1 failed-1. XMASession is corrupted\n");
                    continue;
                }
                if (itr1.session_signature != (void*)(((uint64_t)priv1) | ((uint64_t)priv1->reserved))) {
                    xma_logmsg(XMA_ERROR_LOG, XMAAPI_MOD, "XMA thread1 failed-2. XMASession is corrupted\n");
                    continue;
                }

                XmaHwDevice *dev_tmp1 = priv1->device;
                if (dev_tmp1 == NULL) {
                    xma_logmsg(XMA_ERROR_LOG, XMAAPI_MOD, "XMA thread1 failed-3. Session XMA private pointer is NULL\n");
                    continue;
                }
                if (priv1->kernel_complete_total > 127) {
                    if (priv1->cmd_busy > session_cmd_busiest_val) {
                        session_cmd_busiest_val = priv1->cmd_busy;
                        slowest_session = priv1;
                    }
                }
                priv1->slowest_element = false;
                if (priv1->num_samples > STATS_WINDOW_1) {
                    //xma_logmsg(XMA_ERROR_LOG, XMAAPI_MOD, "stats div: %d, %d, %d\n", (uint32_t)priv1->cmd_busy, (uint32_t)priv1->cmd_idle, (uint32_t)priv1->num_cu_cmds_avg);
                    priv1->cmd_busy = priv1->cmd_busy >> 1;
                    priv1->cmd_idle = priv1->cmd_idle >> 1;
                    //As we need avg cmds in floating point so not taking avg here
                    priv1->num_cu_cmds_avg += priv1->num_cu_cmds_avg_tmp;
                    priv1->num_cu_cmds_avg = priv1->num_cu_cmds_avg >> 1;
                    priv1->num_cu_cmds_avg_tmp = 0;
                    priv1->num_samples = 0;
                    priv1->kernel_complete_total = priv1->kernel_complete_total >> 1;//Even though it is not atomic operation
                } else if (priv1->num_cu_cmds_avg == 0 && priv1->num_samples == 128) {
                    xma_logmsg(XMA_INFO_LOG, "XMA-Session-Stats-Startup", "Session id: %d, type: %s, avg cmds: %.2f, busy vs idle: %d vs %d", itr1.session_id, xma_core::get_session_name(itr1.session_type).c_str(), priv1->num_cu_cmds_avg_tmp / 128.0, (uint32_t)priv1->cmd_busy, (uint32_t)priv1->cmd_idle);
                }
                num_cmds = priv1->num_cu_cmds;
                priv1->num_cu_cmds_avg_tmp += num_cmds;
                if (num_cmds != 0) {
                    if (priv1->cmd_idle_ticks_tmp > priv1->cmd_idle_ticks) {
                        priv1->cmd_idle_ticks = (uint32_t)priv1->cmd_idle_ticks_tmp;
                    }
                    priv1->cmd_idle_ticks_tmp = 0;

                    priv1->cmd_busy_ticks_tmp++;
                    priv1->cmd_busy++;
                    priv1->num_samples++;
                } else if (priv1->cmd_busy != 0) {
                    if (priv1->cmd_busy_ticks_tmp > priv1->cmd_busy_ticks) {
                        priv1->cmd_busy_ticks = (uint32_t)priv1->cmd_busy_ticks_tmp;
                    }
                    priv1->cmd_busy_ticks_tmp = 0;

                    priv1->cmd_idle_ticks_tmp++;
                    priv1->cmd_idle++;
                    priv1->num_samples++;
                }
                XmaHwKernel* kernel_info = priv1->kernel_info;
                if (kernel_info == NULL) {//ADMIN session has no kernel_info
                    continue;
                }
                if (!kernel_info->is_shared) {
                    continue;
                }
                if (kernel_info->num_samples_tmp == kernel_info->num_sessions) {
                    if (kernel_info->cu_busy_tmp != 0) {
                        kernel_info->cu_busy++;
                        kernel_info->num_samples++;
                    }  else if (kernel_info->cu_busy != 0) {
                        kernel_info->cu_idle++;
                        kernel_info->num_samples++;
                    }
                    kernel_info->cu_busy_tmp = 0;
                    kernel_info->num_samples_tmp = 0;
                }
                kernel_info->num_samples_tmp++;
                kernel_info->num_cu_cmds_avg_tmp += num_cmds;
                if (num_cmds != 0) {
                    kernel_info->cu_busy_tmp++;
                }
                if (kernel_info->num_samples > STATS_WINDOW_1) {
                    kernel_info->cu_busy = kernel_info->cu_busy >> 1;
                    kernel_info->cu_idle = kernel_info->cu_idle >> 1;
                    //As we need avg cmds in floating point so not taking avg here
                    kernel_info->num_cu_cmds_avg += kernel_info->num_cu_cmds_avg_tmp;
                    kernel_info->num_cu_cmds_avg = kernel_info->num_cu_cmds_avg >> 1;
                    kernel_info->num_cu_cmds_avg_tmp = 0;
                    kernel_info->num_samples = 0;
                } else if (kernel_info->num_cu_cmds_avg == 0 && kernel_info->num_samples == 128) {
                    xma_logmsg(XMA_INFO_LOG, "XMA-Session-Stats-Startup", "Session id: %d, type: %s, cu: %s, avg cmds: %.2f, busy vs idle: %d vs %d", itr1.session_id, xma_core::get_session_name(itr1.session_type).c_str(), kernel_info->name, kernel_info->num_cu_cmds_avg_tmp / 128.0, (uint32_t)kernel_info->cu_busy, (uint32_t)kernel_info->cu_idle);
                }
            }
            if (slowest_session) {
                slowest_session->slowest_element = true;
            }
        }
    }
    //Print all stats here
    std::lock_guard lock(g_xma_singleton->m_mutex);

    xrt_core::message::send(xrt_core::message::severity_level::info,
                            "XMA-Session-Stats", "=== Session CU Command Relative Stats: ===");
    for (auto& itr1: g_xma_singleton->all_sessions_vec) {
        xrt_core::message::send(xrt_core::message::severity_level::info, "XMA-Session-Stats", "--------");
        XmaHwSessionPrivate *priv1 = (XmaHwSessionPrivate*) itr1.hw_session.private_do_not_use;
        if (priv1->kernel_complete_count != 0 && !priv1->using_cu_cmd_status) {
            xrt_core::message::send(xrt_core::message::severity_level::info,
                                    "XMA-Session-Stats", "Session id: %d, type: %s still has unused completd cu cmds",
                                    itr1.session_id, xma_core::get_session_name(itr1.session_type).c_str());
        }
        float avg_cmds = 0;
        if (priv1->num_cu_cmds_avg != 0) {
            avg_cmds = priv1->num_cu_cmds_avg / STATS_WINDOW;
        } else if (priv1->num_samples > 0) {
            avg_cmds = priv1->num_cu_cmds_avg_tmp / ((float)priv1->num_samples);
        }
        xrt_core::message::send(
            xrt_core::message::severity_level::info,
            "XMA-Session-Stats", "Session id: %d, type: %s, avg cu cmds: %.2f, busy vs idle: %d vs %d",
            itr1.session_id,
            xma_core::get_session_name(itr1.session_type).c_str(),
            avg_cmds,
            (uint32_t)priv1->cmd_busy,
            (uint32_t)priv1->cmd_idle);

        xrt_core::message::send(
            xrt_core::message::severity_level::info,
            "XMA-Session-Stats", "Session id: %d, max busy vs idle ticks: %d vs %d, relative cu load: %d",
            itr1.session_id,
            (uint32_t)priv1->cmd_busy_ticks,
            (uint32_t)priv1->cmd_idle_ticks,
            (uint32_t)priv1->kernel_complete_total);

        XmaHwKernel* kernel_info = priv1->kernel_info;
        if (kernel_info == NULL) {
            continue;
        }
        if (!kernel_info->is_shared) {
            continue;
        }
        if (kernel_info->num_cu_cmds_avg != 0) {
            avg_cmds = kernel_info->num_cu_cmds_avg / STATS_WINDOW;
        } else if (kernel_info->num_samples > 0) {
            avg_cmds = kernel_info->num_cu_cmds_avg_tmp / ((float)kernel_info->num_samples);
        }
        xrt_core::message::send(
            xrt_core::message::severity_level::info,
            "XMA-Session-Stats", "Session id: %d, cu: %s, avg cmds: %.2f, busy vs idle: %d vs %d",
            itr1.session_id,
            kernel_info->name,
            avg_cmds,
            (uint32_t)kernel_info->cu_busy,
            (uint32_t)kernel_info->cu_idle);
    }
    xrt_core::message::send(xrt_core::message::severity_level::info, "XMA-Session-Stats", "--------");
    xrt_core::message::send(xrt_core::message::severity_level::info,
			    "XMA-Session-Stats", "Num of Decoders: %d",
			    (uint32_t)g_xma_singleton->num_decoders);
    xrt_core::message::send(xrt_core::message::severity_level::info,
			    "XMA-Session-Stats", "Num of Scalers: %d",
			    (uint32_t)g_xma_singleton->num_scalers);
    xrt_core::message::send(xrt_core::message::severity_level::info,
			    "XMA-Session-Stats", "Num of Encoders: %d",
			    (uint32_t)g_xma_singleton->num_encoders);
    xrt_core::message::send(xrt_core::message::severity_level::info,
			    "XMA-Session-Stats", "Num of Filters: %d",
			    (uint32_t)g_xma_singleton->num_filters);
    xrt_core::message::send(xrt_core::message::severity_level::info,
			    "XMA-Session-Stats", "Num of Kernels: %d",
			    (uint32_t)g_xma_singleton->num_kernels);
    xrt_core::message::send(xrt_core::message::severity_level::info,
			    "XMA-Session-Stats", "Num of Admins: %d",
			    (uint32_t)g_xma_singleton->num_admins);
    xrt_core::message::send(xrt_core::message::severity_level::info, "XMA-Session-Stats", "--------\n");
}

void xma_thread2(uint32_t hw_dev_index) {
    std::promise<bool> p;
    g_xma_singleton->all_thread2_futures.emplace_back(p.get_future());
    p.set_value_at_thread_exit(true);

    bool expected = false;
    bool desired = true;
    auto xrt_device_obj = g_xma_singleton->hwcfg.devices[hw_dev_index].xrt_device;
    while (!g_xma_singleton->xma_exit) {
        if (g_xma_singleton->cpu_mode == XMA_CPU_MODE2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        } else {
            xrt_device_obj.get_handle()->exec_wait(100);
        }

        std::lock_guard<std::mutex> lock(g_xma_singleton->m_mutex);
        for (auto& itr1: g_xma_singleton->all_sessions_vec) {
            if (g_xma_singleton->xma_exit) {
                break;
            }
            XmaHwSessionPrivate *priv1 = (XmaHwSessionPrivate*) itr1.hw_session.private_do_not_use;
            expected = false;
/*
            while (!priv1->execbo_locked.compare_exchange_weak(expected, desired)) {
                std::this_thread::yield();
                expected = false;
            }
*/
            if (!priv1->execbo_locked.compare_exchange_weak(expected, desired)) {
                continue;
            }
            //execbo lock acquired

            if (xma_core::utils::check_all_execbo(itr1) != XMA_SUCCESS) {
                xma_logmsg(XMA_ERROR_LOG, XMAAPI_MOD, "XMA thread2 failed-4. Unexpected error\n");
                //Release execbo lock
                priv1->execbo_locked = false;
                continue;
            }

            //Release execbo lock
            priv1->execbo_locked = false;
        }
    }
}

void xma_get_session_cmd_load() {
    xma_core::utils::get_session_cmd_load();
}

//Return num of xilinx devices on x86 host
int32_t xma_num_devices() {
    int32_t ret = XMA_ERROR;
    if (!g_xma_singleton->xma_initialized) {
        ret = xma_core::utils::load_libxrt();
        if (ret == XMA_ERROR) {
            std::cout << "XMA FATAL: Unable to load XRT library" << std::endl;
            return XMA_ERROR;
        }
    }
    return xrt_core::get_total_devices(true).first;
}

int32_t xma_initialize(XmaXclbinParameter *devXclbins, int32_t num_parms)
{
    int32_t ret;
    //bool    rc;

    if (g_xma_singleton == NULL) {
        std::cout << "XMA FATAL: Singleton is NULL" << std::endl;
        return XMA_ERROR;
    }
    if (num_parms < 1) {
        std::cout << "XMA FATAL: Must provide atleast one XmaXclbinParameter." << std::endl;
        return XMA_ERROR;
    }

    std::lock_guard<std::mutex> guard1(g_xma_singleton->m_mutex);
    //Singleton lock acquired

    if (g_xma_singleton->xma_initialized) {
        std::cout << "XMA FATAL: XMA is already initialized" << std::endl;

        return XMA_ERROR;
    }

    int32_t xrtlib = xma_core::utils::load_libxrt();
    switch(xrtlib) {
        case XMA_ERROR:
          std::cout << "XMA FATAL: Unable to load XRT library" << std::endl;
          return XMA_ERROR;
          break;
        case 1:
          xma_logmsg(XMA_INFO_LOG, XMAAPI_MOD, "Loaded xrt_core libary\n");
          break;
        case 2:
          xma_logmsg(XMA_INFO_LOG, XMAAPI_MOD, "Loaded xrt_aws libary\n");
          break;
        case 3:
          xma_logmsg(XMA_INFO_LOG, XMAAPI_MOD, "Loaded user supplied xrt_hwem libary\n");
          break;
        case 4:
          xma_logmsg(XMA_INFO_LOG, XMAAPI_MOD, "Loaded user supplied xrt_swem libary\n");
          break;
        case 5:
          xma_logmsg(XMA_INFO_LOG, XMAAPI_MOD, "Loaded installed xrt_hwem libary\n");
          break;
        case 6:
          xma_logmsg(XMA_INFO_LOG, XMAAPI_MOD, "Loaded installed xrt_swem libary\n");
          break;
        default:
          std::cout << "XMA FATAL: Unexpected error. Unable to load XRT library" << std::endl;
          return XMA_ERROR;
          break;
    }

    g_xma_singleton->hwcfg.devices.reserve(MAX_XILINX_DEVICES);
   
    xma_logmsg(XMA_INFO_LOG, XMAAPI_MOD, "Probing hardware\n");
    ret = xma_hw_probe(&g_xma_singleton->hwcfg);
    if (ret != XMA_SUCCESS) {
        for (XmaHwDevice& hw_device: g_xma_singleton->hwcfg.devices) {
            hw_device.kernels.clear();
        }
        g_xma_singleton->hwcfg.devices.clear();
        g_xma_singleton->hwcfg.num_devices = -1;

        return ret;
    }

    xma_logmsg(XMA_INFO_LOG, XMAAPI_MOD, "Configure hardware\n");
    if (!xma_hw_configure(&g_xma_singleton->hwcfg, devXclbins, num_parms)) {
        for (XmaHwDevice& hw_device: g_xma_singleton->hwcfg.devices) {
            hw_device.kernels.clear();
        }
        g_xma_singleton->hwcfg.devices.clear();
        g_xma_singleton->hwcfg.num_devices = -1;

        return XMA_ERROR;
    }

    int32_t exec_mode = xrt_core::config::get_xma_exec_mode();
    switch (exec_mode) {
        case 1:
            g_xma_singleton->num_execbos = XMA_NUM_EXECBO_DEFAULT;
            xma_logmsg(XMA_DEBUG_LOG, XMAAPI_MOD, "XMA Exec Mode-1: Max of %d cu cmd per session", XMA_NUM_EXECBO_DEFAULT);
            break;
        case 2:
            g_xma_singleton->num_execbos = XMA_NUM_EXECBO_MODE2;
            xma_logmsg(XMA_DEBUG_LOG, XMAAPI_MOD, "XMA Exec Mode-2: Max of %d cu cmd per session", XMA_NUM_EXECBO_MODE2);
            break;
        case 3:
            g_xma_singleton->num_execbos = XMA_NUM_EXECBO_MODE3;
            xma_logmsg(XMA_DEBUG_LOG, XMAAPI_MOD, "XMA Exec Mode-3: Max of %d cu cmd per session", XMA_NUM_EXECBO_MODE3);
            break;
        case 4:
            g_xma_singleton->num_execbos = XMA_NUM_EXECBO_MODE4;
            xma_logmsg(XMA_DEBUG_LOG, XMAAPI_MOD, "XMA Exec Mode-4: Max of %d cu cmd per session", XMA_NUM_EXECBO_MODE4);
            break;
        default:
            g_xma_singleton->num_execbos = XMA_NUM_EXECBO_DEFAULT;
            xma_logmsg(XMA_DEBUG_LOG, XMAAPI_MOD, "XMA Exec Mode-1: Max of %d cu cmd per session", XMA_NUM_EXECBO_DEFAULT);
            break;
    }

    g_xma_singleton->kds_old = xrt_core::config::get_xma_kds_old();
    if (g_xma_singleton->kds_old)
        xma_logmsg(XMA_DEBUG_LOG, XMAAPI_MOD, "XMA for old KDS.");
    else
        xma_logmsg(XMA_DEBUG_LOG, XMAAPI_MOD, "XMA for KDS 2.0. Default mode.");
    g_xma_singleton->cpu_mode = xrt_core::config::get_xma_cpu_mode();
    xma_logmsg(XMA_DEBUG_LOG, XMAAPI_MOD, "XMA CPU Mode is: %d", g_xma_singleton->cpu_mode.load());

    g_xma_singleton->xma_thread1 = std::thread(xma_thread1);
    g_xma_singleton->all_thread2.reserve(MAX_XILINX_DEVICES);
    g_xma_singleton->all_thread2_futures.reserve(MAX_XILINX_DEVICES);
    uint32_t num_devices = g_xma_singleton->hwcfg.devices.size();
    for (uint32_t i = 0; i < num_devices; i++) {
        g_xma_singleton->all_thread2.emplace_back(std::thread(xma_thread2, i));
        g_xma_singleton->all_thread2.back().detach();
    }
    //Detach threads to let them run independently
    g_xma_singleton->xma_thread1.detach();

    g_xma_singleton->xma_initialized = true;
    return XMA_SUCCESS;
}

void xma_exit(void)
{
    if (!g_xma_singleton) {
        return;
    }
    g_xma_singleton->xma_exit = true;
    if (!g_xma_singleton->xma_initialized) {
        return;
    }
    try {
        if (g_xma_singleton->thread1_future.valid())
            g_xma_singleton->thread1_future.wait();
    } catch (...) {}
    try {
        for (const auto& thread2_f: g_xma_singleton->all_thread2_futures) {
            if (thread2_f.valid())
              thread2_f.wait();
        }
    } catch (...) {}
}

