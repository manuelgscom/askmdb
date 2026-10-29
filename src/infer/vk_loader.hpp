#pragma once
// Vulkan is loaded at run time (dlopen of libvulkan.so.1), never linked: the binary starts and
// runs on the CPU on machines without a Vulkan loader. Built with ASKMDB_VULKAN=OFF, Vulkan is
// never available and these functions do nothing.

namespace askmdb::infer {

// Whether this binary was built with the Vulkan backend.
bool vulkan_built();

// Call before the first use of ggml/llama.cpp: with false, the Vulkan backend finds no loader and
// registers no device (CPU mode pays no Vulkan start-up and never touches any GPU). It has no
// effect once ggml's backend registry exists.
void vulkan_set_enabled(bool enabled);

// Whether libvulkan.so.1 was loaded (false until ggml has tried, or when disabled or missing).
bool vulkan_loaded();

}  // namespace askmdb::infer
