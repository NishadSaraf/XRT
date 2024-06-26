// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2016-2022 Xilinx, Inc.  All rights reserved.
// Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
#include "program.h"
#include "device.h"
#include "kernel.h"
#include "context.h"
#include "platform.h" // for get_install_root
#include "error.h"

#include "xocl/api/plugin/xdp/profile_v2.h"

#include <vector>
#include <iostream>
#include <fstream>
#include <memory>

#ifdef _WIN32
#pragma warning ( disable : 4996 )
#endif

namespace {

} // namespace

namespace xocl {

program::
program(context* ctx, const std::string& source)
  : m_context(ctx), m_source(source)
{
  static unsigned int uid_count = 0;
  m_uid = uid_count++;

  XOCL_DEBUG(std::cout,"xocl::program::program(",m_uid,")\n");
  m_context->add_program(this);
}

program::
program(context* ctx,cl_uint num_devices, const cl_device_id* devices,
        const unsigned char** binaries, const size_t* lengths)
  : program(ctx,"")
{
  for (cl_uint i=0; i<num_devices; ++i) {
    auto device = xocl::xocl(devices[i]);
    m_devices.push_back(device);
    m_binaries.emplace(device,std::vector<char>{binaries[i], binaries[i] + lengths[i]});
  }

  // Verify that each binary contains the same kernels
  // Well, let's not bother verifying, let runtime fail later
}

program::
~program()
{
  XOCL_DEBUG(std::cout,"xocl::program::~program(",m_uid,")\n");

  try {
    // Before deleting program, do a final read of counters
    // and force flush of trace buffers
    xocl::profile::mark_objects_released() ;

    for(auto d : get_device_range())
    {
      xocl::profile::flush_device(d->get_xdevice());
      d->unload_program(this);
    }

    m_context->remove_program(this);
  }
  catch (...) {}
}

void
program::
add_device(device* d)
{
  m_devices.push_back(d);
}

program::target_type
program::
get_target() const
{
  if (auto metadata = get_xclbin(nullptr))
    return metadata.target();
  throw std::runtime_error("No program metadata");
}

xclbin
program::
get_xclbin(const device* d) const
{
  // switch to parent device if any
  d = d ? d->get_root_device() : nullptr;
  if (d) {
    auto itr = m_binaries.find(d);
    if (itr==m_binaries.end())
      throw xocl::error(CL_INVALID_DEVICE,"No binary for device");

    return d->get_xclbin();
  }

  if (auto device = get_first_device())
    return device->get_xclbin();

  throw xocl::error(CL_INVALID_PROGRAM_EXECUTABLE,"No binary for program");
}

xrt_core::uuid
program::
get_xclbin_uuid(const device* d) const
{
  // switch to parent device if necessary
  d = d->get_root_device();
  auto itr = m_binaries.find(d);
  if (itr == m_binaries.end())
    return {};

  auto top = reinterpret_cast<const axlf*>((*itr).second.data());
  return top->m_header.uuid;
}

std::pair<const char*, const char*>
program::
get_xclbin_binary(const device* d) const
{
  // switch to parent device if necessary
  d = d->get_root_device();
  auto itr = m_binaries.find(d);
  if (itr==m_binaries.end())
    throw xocl::error(CL_INVALID_DEVICE,"No binary for device");

  return {(*itr).second.data(), (*itr).second.data() + (*itr).second.size()};
}

std::vector<size_t>
program::
get_binary_sizes() const
{
  std::vector<size_t> sizes;
  // It's important to iterate the binaries in the device range order
  // because clGetProgramInfo relies on binary sizes to match the
  // binaries returned by iterating device range
  for (auto& device : m_devices) {
    auto xclbin = get_xclbin_binary(device.get());
    sizes.push_back(xclbin.second - xclbin.first);
  }
  return sizes;
}

unsigned int
program::
get_num_kernels() const
{
  if (auto metadata = get_xclbin(nullptr))
    return metadata.num_kernels();
  return 0;
}

std::vector<std::string>
program::
get_kernel_names() const
{
  if (auto metadata = get_xclbin(nullptr))
    return metadata.kernel_names();
  return {};
}

bool
program::
has_kernel(const std::string& kname) const
{
  auto name = kernel_utils::normalize_kernel_name(kname);
  auto kernels = get_kernel_names();
  return range_find(kernels,[&name](const std::string& s){return s==name;})!=kernels.end();
}

std::unique_ptr<kernel,std::function<void(kernel*)>>
program::
create_kernel(const std::string& kernel_name)
{
  auto deleter = [](kernel* k) { k->release(); };

  // Look up kernel symbol from first matching xclbin
  if (m_binaries.empty())
    throw xocl::error(CL_INVALID_PROGRAM_EXECUTABLE,"No binary for program");

  // Find first matching kernel in any device program
  auto normalized_kernel_name = kernel_utils::normalize_kernel_name(kernel_name);
  for (const auto& device : m_devices) {
    const auto& xclbin = device->get_xrt_xclbin();
    auto xk = xclbin.get_kernel(normalized_kernel_name);
    if (!xk)
      continue;

    auto k = std::make_unique<kernel>(this, kernel_name, xk);
    return std::unique_ptr<kernel,decltype(deleter)>(k.release(), deleter);
  }

  throw xocl::error(CL_INVALID_PROGRAM_EXECUTABLE,"Kernel not found");

}

program::creation_type
program::
get_creation_type() const
{
  if (!m_source.empty())
    return creation_type::source;
  else if (m_options.empty() && m_logs.empty() && !m_binaries.empty())
    return creation_type::binary;
  else
    throw xocl::error(CL_INVALID_PROGRAM,"Cannot determine source of program");
}

void
program::
build(const std::vector<device*>&,const std::string&)
{
  throw std::runtime_error("build program is not safe and no longer supported");
}

} // xocl
