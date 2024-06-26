/**
 * Copyright (C) 2022 Xilinx, Inc
 * Copyright (C) 2023 Advanced Micro Devices, Inc. - All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/device_offload/hw_emu/hw_emu_device_offload_cb.h"
#include "xdp/profile/plugin/device_offload/hw_emu/hw_emu_device_offload_plugin.h"

namespace xdp {

  static HWEmuDeviceOffloadPlugin hwEmuDeviceOffloadPluginInstance ;

} // end namespace xdp

extern "C"
void updateDeviceHWEmu(void* handle)
{
  xdp::hwEmuDeviceOffloadPluginInstance.updateDevice(handle) ;
}

extern "C"
void flushDeviceHWEmu(void* handle)
{
  xdp::hwEmuDeviceOffloadPluginInstance.flushDevice(handle) ;
}
