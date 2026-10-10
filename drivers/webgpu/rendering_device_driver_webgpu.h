/**************************************************************************/
/*  rendering_device_driver_webgpu.h                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#ifdef WEBGPU_ENABLED

#include "core/templates/local_vector.h"
#include "drivers/webgpu/rendering_context_driver_webgpu.h"
#include "drivers/webgpu/rendering_shader_container_webgpu.h"
#include "servers/rendering/rendering_device_driver.h"

#include "drivers/webgpu/webgpu_wait.h"

class RenderingDeviceDriverWebGPU : public RenderingDeviceDriver {
	GDSOFTCLASS(RenderingDeviceDriverWebGPU, RenderingDeviceDriver);

public:
	// ----- Resource bookkeeping. Opaque driver IDs are pointers to these. -----

	struct BufferInfo {
		WGPUBuffer buffer = nullptr;
		uint64_t size = 0;
		BitField<BufferUsageBits> usage = {};
		// CPU buffers have a shadow copy: WebGPU has no persistently mapped memory.
		bool cpu = false;
		bool download = false;
		bool gpu_written = false;
		Vector<uint8_t> shadow;
		// BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT: `frame_count` regions of `size` bytes each, one per frame in flight. The
		// shadow holds all regions; buffer_flush() uploads the current one.
		bool dynamic = false;
		uint32_t frame_idx = 0;
		uint64_t total_size = 0;
	};

	struct TextureInfo {
		WGPUTexture texture = nullptr;
		WGPUTextureView view = nullptr;
		bool owns_texture = true;
		DataFormat format = DATA_FORMAT_MAX;
		WGPUTextureFormat wgpu_format = WGPUTextureFormat_Undefined;
		TextureType type = TEXTURE_TYPE_2D;
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;
		uint32_t layers = 1;
		uint32_t mipmaps = 1;
		uint32_t samples = 1;
		uint32_t usage_bits = 0;
		uint64_t allocation_size = 0;
		// Parameters of the default view.
		WGPUTextureViewDimension view_dimension = WGPUTextureViewDimension_2D;
		uint32_t base_mip = 0;
		uint32_t view_mip_count = 1;
		uint32_t base_layer = 0;
		uint32_t view_layer_count = 1;
	};

	struct SamplerInfo {
		WGPUSampler sampler = nullptr;
		WGPUSampler nearest_sampler = nullptr; // Only for filtering samplers.
		bool comparison = false;
		bool linear = false;
	};

	struct ShaderInfo {
		String name;
		ShaderReflection reflection;
		Vector<RenderingShaderContainerWebGPU::BindingExtra> binding_extras; // Flattened over all sets.
		LocalVector<WGPUBindGroupLayout> set_layouts;
		WGPUPipelineLayout pipeline_layout = nullptr;
		WGPUShaderModule modules[SHADER_STAGE_MAX] = {};
		// The overridable constants (specialization constants) each WGSL module declares. A stage rejects the ids it does not know.
		LocalVector<uint32_t> override_ids[SHADER_STAGE_MAX];
		bool has_override_ids[SHADER_STAGE_MAX] = {};
		// The extras of the bindings by (set << 32 | binding), to adapt textures to the layout when a uniform set is created.
		HashMap<uint64_t, RenderingShaderContainerWebGPU::BindingExtra> extras_by_binding;
		uint32_t push_constant_size = 0;
		// WGSL shaders keep push constants in a uniform buffer in group 0 (see command_bind_push_constants).
		bool emulate_push_constants = false;
		WGPUBindGroup default_group0 = nullptr; // Group 0 for shaders that have no uniform set 0 of their own.
	};

	struct UniformSetInfo {
		WGPUBindGroup bind_group = nullptr;
		LocalVector<const BufferInfo *> dynamic_buffers; // Ordered by binding, like the dynamic offsets of WebGPU.
	};

	struct PipelineInfo {
		WGPUComputePipeline compute = nullptr;
		WGPURenderPipeline render = nullptr;
		ShaderInfo *shader = nullptr;
		uint32_t stencil_reference = 0;
	};

	struct RenderPassInfo {
		LocalVector<Attachment> attachments;
		LocalVector<Subpass> subpasses;
	};

	struct FramebufferInfo {
		LocalVector<TextureInfo *> attachments;
		RenderPassInfo *render_pass = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct VertexFormatInfo {
		LocalVector<WGPUVertexBufferLayout> layouts;
		LocalVector<LocalVector<WGPUVertexAttribute>> attributes;
	};

	struct SwapChainInfo {
		RenderingContextDriver::SurfaceID surface_id = 0;
		WGPUSurface surface = nullptr;
		WGPUTextureFormat wgpu_format = WGPUTextureFormat_Undefined;
		DataFormat format = DATA_FORMAT_MAX;
		RenderPassInfo *render_pass = nullptr;
		bool configured = false;
		uint32_t width = 0;
		uint32_t height = 0;
		// The image acquired for the frame that is being recorded, presented by command_queue_execute_and_present().
		TextureInfo *current_texture = nullptr;
		FramebufferInfo *current_framebuffer = nullptr;
	};

	struct FenceInfo {
		WGPUFuture future = {};
		volatile bool done = false;
		bool pending = false;
	};

	struct CommandBufferInfo {
		WGPUCommandEncoder encoder = nullptr;
		WGPUComputePassEncoder compute_pass = nullptr;
		WGPURenderPassEncoder render_pass = nullptr;
		LocalVector<WGPUTextureView> temporary_views;
		WGPUCommandBuffer finished = nullptr;
		// Push constant emulation: group 0 is bound lazily, right before a draw or dispatch, with the dynamic offset of
		// the latest push constant slot.
		const ShaderInfo *shader = nullptr;
		WGPUBindGroup group0 = nullptr;
		uint32_t push_offset = 0;
		bool group0_dirty = false;
		LocalVector<uint32_t> group0_dynamic_offsets; // Offsets of the dynamic buffers of set 0, then the push constant slot.
		uint8_t push_shadow[RenderingShaderContainerWebGPU::PUSH_CONSTANT_SLOT_SIZE] = {};
	};

private:
	RenderingContextDriverWebGPU *context_driver = nullptr;
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPULimits limits = {};
	bool immediates_supported = false;
	uint32_t frame_count = 1;
	WGPUBuffer push_constant_ring = nullptr;
	uint32_t push_constant_next_slot = 0;
	static constexpr uint32_t PUSH_CONSTANT_SLOTS = 16384;
	uint64_t total_memory_used = 0;

	RenderingShaderContainerFormatWebGPU shader_container_format;
	Capabilities capabilities;
	MultiviewCapabilities multiview_capabilities;
	FragmentShadingRateCapabilities fsr_capabilities;
	FragmentDensityMapCapabilities fdm_capabilities;

	void _wait_for(WGPUFuture p_future, const volatile bool &p_done);
	void _end_compute_pass(CommandBufferInfo *p_cmd);
	void _ensure_compute_pass(CommandBufferInfo *p_cmd);
	void _end_render_pass(CommandBufferInfo *p_cmd);
	void _flush_group0(CommandBufferInfo *p_cmd, bool p_compute);
	// The view to bind: the texture's own, or one with the dimension the shader layout expects (engines bind default
	// textures of another kind, a cubemap for a 2D slot for example, which Vulkan tolerates).
	WGPUSampler _sampler_for_binding(const SamplerInfo *p_sampler, const ShaderInfo *p_shader, uint32_t p_set, uint32_t p_binding, bool p_combined);
	WGPUTextureView _view_for_binding(const TextureInfo *p_texture, const ShaderInfo *p_shader, uint32_t p_set, uint32_t p_binding, LocalVector<WGPUTextureView> &r_temporary_views);
	// Builds the constants of a pipeline stage; the keys stay valid as long as p_keys lives.
	void _build_pipeline_constants(const ShaderInfo *p_shader, ShaderStage p_stage, VectorView<PipelineSpecializationConstant> p_specialization_constants, LocalVector<CharString> &p_keys, LocalVector<WGPUConstantEntry> &r_constants);
	void _release_swap_chain_image(SwapChainInfo *p_swap_chain);
	bool _map_for_read(BufferInfo *p_buffer);

public:
	WGPUDevice device_get() const { return device; }

	virtual uint64_t api_trait_get(ApiTrait p_trait) override;
	virtual void buffer_flush(BufferID p_buffer) override;
	virtual uint32_t shader_get_layout_hash(ShaderID p_shader) override { return 0; }

	virtual Error initialize(uint32_t p_device_index, uint32_t p_frame_count) override;
	virtual BufferID buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) override;
	virtual bool buffer_set_texel_format(BufferID p_buffer, DataFormat p_format) override;
	virtual void buffer_free(BufferID p_buffer) override;
	virtual uint64_t buffer_get_allocation_size(BufferID p_buffer) override;
	virtual uint8_t *buffer_map(BufferID p_buffer) override;
	virtual void buffer_unmap(BufferID p_buffer) override;
	virtual uint8_t *buffer_persistent_map_advance(BufferID p_buffer, uint64_t p_frames_drawn) override;
	virtual uint64_t buffer_get_dynamic_offsets(Span<BufferID> p_buffers) override;
	virtual uint64_t buffer_get_device_address(BufferID p_buffer) override;
	virtual TextureID texture_create(const TextureFormat &p_format, const TextureView &p_view) override;
	virtual TextureID texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool p_depth_stencil, uint32_t p_mipmaps) override;
	virtual TextureID texture_create_shared(TextureID p_original_texture, const TextureView &p_view) override;
	virtual TextureID texture_create_shared_from_slice(TextureID p_original_texture, const TextureView &p_view, TextureSliceType p_slice_type, uint32_t p_layer, uint32_t p_layers, uint32_t p_mipmap, uint32_t p_mipmaps) override;
	virtual void texture_free(TextureID p_texture) override;
	virtual uint64_t texture_get_allocation_size(TextureID p_texture) override;
	virtual void texture_get_copyable_layout(TextureID p_texture, const TextureSubresource &p_subresource, TextureCopyableLayout *r_layout) override;
	virtual Vector<uint8_t> texture_get_data(TextureID p_texture, uint32_t p_layer) override;
	virtual BitField<TextureUsageBits> texture_get_usages_supported_by_format(DataFormat p_format, bool p_cpu_readable) override;
	virtual bool texture_can_make_shared_with_format(TextureID p_texture, DataFormat p_format, bool &r_raw_reinterpretation) override;
	virtual SamplerID sampler_create(const SamplerState &p_state) override;
	virtual void sampler_free(SamplerID p_sampler) override;
	virtual bool sampler_is_format_supported_for_filter(DataFormat p_format, SamplerFilter p_filter) override;
	virtual VertexFormatID vertex_format_create(Span<VertexAttribute> p_vertex_attribs, const VertexAttributeBindingsMap &p_vertex_bindings) override;
	virtual void vertex_format_free(VertexFormatID p_vertex_format) override;
	virtual void command_pipeline_barrier(CommandBufferID p_cmd_buffer, BitField<PipelineStageBits> p_src_stages, BitField<PipelineStageBits> p_dst_stages, VectorView<MemoryAccessBarrier> p_memory_barriers, VectorView<BufferBarrier> p_buffer_barriers, VectorView<TextureBarrier> p_texture_barriers, VectorView<AccelerationStructureBarrier> p_acceleration_structure_barriers) override;
	virtual FenceID fence_create() override;
	virtual Error fence_wait(FenceID p_fence) override;
	virtual void fence_free(FenceID p_fence) override;
	virtual SemaphoreID semaphore_create() override;
	virtual void semaphore_free(SemaphoreID p_semaphore) override;
	virtual CommandQueueFamilyID command_queue_family_get(BitField<CommandQueueFamilyBits> p_cmd_queue_family_bits, RenderingContextDriver::SurfaceID p_surface) override;
	virtual CommandQueueID command_queue_create(CommandQueueFamilyID p_cmd_queue_family, bool p_identify_as_main_queue = false) override;
	virtual Error command_queue_execute_and_present(CommandQueueID p_cmd_queue, VectorView<SemaphoreID> p_wait_semaphores, VectorView<CommandBufferID> p_cmd_buffers, VectorView<SemaphoreID> p_cmd_semaphores, FenceID p_cmd_fence, VectorView<SwapChainID> p_swap_chains) override;
	virtual void command_queue_free(CommandQueueID p_cmd_queue) override;
	virtual CommandPoolID command_pool_create(CommandQueueFamilyID p_cmd_queue_family, CommandBufferType p_cmd_buffer_type) override;
	virtual bool command_pool_reset(CommandPoolID p_cmd_pool) override;
	virtual void command_pool_free(CommandPoolID p_cmd_pool) override;
	virtual CommandBufferID command_buffer_create(CommandPoolID p_cmd_pool) override;
	virtual bool command_buffer_begin(CommandBufferID p_cmd_buffer) override;
	virtual bool command_buffer_begin_secondary(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, uint32_t p_subpass, FramebufferID p_framebuffer) override;
	virtual void command_buffer_end(CommandBufferID p_cmd_buffer) override;
	virtual void command_buffer_execute_secondary(CommandBufferID p_cmd_buffer, VectorView<CommandBufferID> p_secondary_cmd_buffers) override;
	virtual SwapChainID swap_chain_create(RenderingContextDriver::SurfaceID p_surface) override;
	virtual Error swap_chain_resize(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, uint32_t p_desired_framebuffer_count) override;
	virtual FramebufferID swap_chain_acquire_framebuffer(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, bool &r_resize_required) override;
	virtual RenderPassID swap_chain_get_render_pass(SwapChainID p_swap_chain) override;
	virtual DataFormat swap_chain_get_format(SwapChainID p_swap_chain) override;
	virtual ColorSpace swap_chain_get_color_space(SwapChainID p_swap_chain) override;
	virtual bool swap_chain_get_hdr_output_supported(SwapChainID p_swap_chain) override;
	virtual void swap_chain_free(SwapChainID p_swap_chain) override;
	virtual FramebufferID framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) override;
	virtual void framebuffer_free(FramebufferID p_framebuffer) override;
	virtual ShaderID shader_create_from_container(const Ref<RenderingShaderContainer> &p_shader_container, const Vector<ImmutableSampler> &p_immutable_samplers) override;
	virtual void shader_free(ShaderID p_shader) override;
	virtual void shader_destroy_modules(ShaderID p_shader) override;
	virtual UniformSetID uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader, uint32_t p_set_index, int p_linear_pool_index) override;
	virtual void uniform_set_free(UniformSetID p_uniform_set) override;
	virtual uint32_t uniform_sets_get_dynamic_offsets(VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count) const override;
	virtual void command_uniform_set_prepare_for_use(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) override;
	virtual void command_clear_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, uint64_t p_offset, uint64_t p_size) override;
	virtual void command_copy_buffer(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, BufferID p_dst_buffer, VectorView<BufferCopyRegion> p_regions) override;
	virtual void command_copy_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<TextureCopyRegion> p_regions) override;
	virtual void command_resolve_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) override;
	virtual void command_clear_color_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, const Color &p_color, const TextureSubresourceRange &p_subresources) override;
	virtual void command_clear_depth_stencil_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, float p_depth, uint8_t p_stencil, const TextureSubresourceRange &p_subresources) override;
	virtual void command_copy_buffer_to_texture(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<BufferTextureCopyRegion> p_regions) override;
	virtual void command_copy_texture_to_buffer(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, BufferID p_dst_buffer, VectorView<BufferTextureCopyRegion> p_regions) override;
	virtual void pipeline_free(PipelineID p_pipeline) override;
	virtual void command_bind_push_constants(CommandBufferID p_cmd_buffer, ShaderID p_shader, uint32_t p_first_index, VectorView<uint32_t> p_data) override;
	virtual bool pipeline_cache_create(const Vector<uint8_t> &p_data) override;
	virtual void pipeline_cache_free() override;
	virtual size_t pipeline_cache_query_size() override;
	virtual Vector<uint8_t> pipeline_cache_serialize() override;
	virtual RenderPassID render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses, VectorView<SubpassDependency> p_subpass_dependencies, uint32_t p_view_count, AttachmentReference p_fragment_density_map_attachment) override;
	virtual void render_pass_free(RenderPassID p_render_pass) override;
	virtual void command_begin_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RenderPassClearValue> p_clear_values) override;
	virtual void command_end_render_pass(CommandBufferID p_cmd_buffer) override;
	virtual void command_next_render_subpass(CommandBufferID p_cmd_buffer, CommandBufferType p_cmd_buffer_type) override;
	virtual void command_render_set_viewport(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_viewports) override;
	virtual void command_render_set_scissor(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_scissors) override;
	virtual void command_render_clear_attachments(CommandBufferID p_cmd_buffer, VectorView<AttachmentClear> p_attachment_clears, VectorView<Rect2i> p_rects) override;
	virtual void command_bind_render_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) override;
	virtual void command_bind_render_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) override;
	virtual void command_render_draw(CommandBufferID p_cmd_buffer, uint32_t p_vertex_count, uint32_t p_instance_count, uint32_t p_base_vertex, uint32_t p_first_instance) override;
	virtual void command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) override;
	virtual void command_render_draw_indexed_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) override;
	virtual void command_render_draw_indexed_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) override;
	virtual void command_render_draw_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) override;
	virtual void command_render_draw_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) override;
	virtual void command_render_bind_vertex_buffers(CommandBufferID p_cmd_buffer, uint32_t p_binding_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) override;
	virtual void command_render_bind_index_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, IndexBufferFormat p_format, uint64_t p_offset) override;
	virtual void command_render_set_blend_constants(CommandBufferID p_cmd_buffer, const Color &p_constants) override;
	virtual void command_render_set_line_width(CommandBufferID p_cmd_buffer, float p_width) override;
	virtual PipelineID render_pipeline_create(ShaderID p_shader, VertexFormatID p_vertex_format, RenderPrimitive p_render_primitive, PipelineRasterizationState p_rasterization_state, PipelineMultisampleState p_multisample_state, PipelineDepthStencilState p_depth_stencil_state, PipelineColorBlendState p_blend_state, VectorView<int32_t> p_color_attachments, BitField<PipelineDynamicStateFlags> p_dynamic_state, RenderPassID p_render_pass, uint32_t p_render_subpass, VectorView<PipelineSpecializationConstant> p_specialization_constants) override;
	virtual void command_bind_compute_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) override;
	virtual void command_bind_compute_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) override;
	virtual void command_compute_dispatch(CommandBufferID p_cmd_buffer, uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) override;
	virtual void command_compute_dispatch_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset) override;
	virtual PipelineID compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants) override;
	virtual AccelerationStructureID blas_create(VectorView<AccelerationStructureGeometry> p_geometries, BitField<AccelerationStructureFlagBits> p_flags) override;
	virtual AccelerationStructureID tlas_create(uint32_t p_max_instance_count, BitField<AccelerationStructureFlagBits> p_flags) override;
	virtual void acceleration_structure_instance_write(uint8_t *r_driver_instance, const AccelerationStructureInstance &p_instance) override;
	virtual void acceleration_structure_free(AccelerationStructureID p_acceleration_structure) override;
	virtual uint32_t acceleration_structure_get_scratch_size_bytes(AccelerationStructureID p_acceleration_structure) override;
	virtual RaytracingPipelineID raytracing_pipeline_create(VectorView<PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, ShaderID p_layout_defining_shader) override;
	virtual void raytracing_pipeline_free(RaytracingPipelineID p_pipeline) override;
	virtual bool raytracing_pipeline_get_shader_group_handles(RaytracingPipelineID p_pipeline, uint32_t p_group_index_offset, VectorView<uint32_t> p_group_indices, uint8_t *r_data, uint32_t p_data_stride_bytes) override;
	virtual void command_build_blas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer) override;
	virtual void command_build_tlas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer, BufferID p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count) override;
	virtual void command_bind_raytracing_pipeline(CommandBufferID p_cmd_buffer, RaytracingPipelineID p_pipeline) override;
	virtual void command_bind_raytracing_uniform_set(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) override;
	virtual void command_trace_rays(CommandBufferID p_cmd_buffer, const ShaderBindingTable &p_raygen_sbt, const ShaderBindingTable &p_miss_sbt, const ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth) override;
	virtual QueryPoolID timestamp_query_pool_create(uint32_t p_query_count) override;
	virtual void timestamp_query_pool_free(QueryPoolID p_pool_id) override;
	virtual void timestamp_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) override;
	virtual uint64_t timestamp_query_result_to_time(uint64_t p_result) override;
	virtual void command_timestamp_query_pool_reset(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_query_count) override;
	virtual void command_timestamp_write(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_index) override;
	virtual void command_begin_label(CommandBufferID p_cmd_buffer, const char *p_label_name, const Color &p_color) override;
	virtual void command_end_label(CommandBufferID p_cmd_buffer) override;
	virtual void command_insert_breadcrumb(CommandBufferID p_cmd_buffer, uint32_t p_data) override;
	virtual void begin_segment(uint32_t p_frame_index, uint32_t p_frames_drawn) override;
	virtual void end_segment() override;
	virtual void set_object_name(ObjectType p_type, ID p_driver_id, const String &p_name) override;
	virtual uint64_t get_resource_native_handle(DriverResource p_type, ID p_driver_id) override;
	virtual uint64_t get_total_memory_used() override;
	virtual uint64_t get_lazily_memory_used() override;
	virtual uint64_t limit_get(Limit p_limit) override;
	virtual bool has_feature(Features p_feature) override;
	virtual const MultiviewCapabilities &get_multiview_capabilities() override;
	virtual const FragmentShadingRateCapabilities &get_fragment_shading_rate_capabilities() override;
	virtual const FragmentDensityMapCapabilities &get_fragment_density_map_capabilities() override;
	virtual String get_api_name() const override;
	virtual String get_api_version() const override;
	virtual String get_pipeline_cache_uuid() const override;
	virtual const Capabilities &get_capabilities() const override;
	virtual const RenderingShaderContainerFormat &get_shader_container_format() const override;

	RenderingDeviceDriverWebGPU(RenderingContextDriverWebGPU *p_context_driver);
	virtual ~RenderingDeviceDriverWebGPU();
};

#endif // WEBGPU_ENABLED
