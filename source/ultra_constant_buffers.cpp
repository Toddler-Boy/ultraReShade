/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <algorithm>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <reshade.hpp>

#include "addon_manager.hpp"

void track_constant_buffers ( bool shouldTrack );

//-----------------------------------------------------------------------------

// The addon_event::ultra_constant_buffer source: follows what a device's buffers hold and what each command list binds,
// and hands the bytes of every changed constant buffer binding over before the draw or dispatch that reads it
namespace
{
	using namespace reshade;

	// What ReShade last saw a buffer hold: a copy (Map/Unmap, UpdateSubresource, a recorded copy), or the persistently
	// mapped memory of an upload buffer the CPU writes without telling anyone (D3D12, Vulkan)
	struct BufferContents
	{
		std::vector<uint8_t>	bytes;
		const uint8_t*			mapped = nullptr;		// the whole buffer, valid until unmapped
		uint64_t				mappedSize = 0;
		uint64_t				version = 1;
	};

	struct __declspec ( uuid ( "6f0b6c2e-8a35-4f6e-9d2b-3c1a0cb00001" ) ) DeviceState
	{
		std::shared_mutex								lock;
		std::unordered_map<uint64_t, BufferContents>	buffers;
		std::unordered_map<uint64_t, void*>				pendingMaps;	// Map/Unmap APIs: the pointer between the two
		std::unordered_map<uint64_t, api::buffer_range>	tableBuffers;	// constant buffer descriptors written into heaps
	};

	// A layout's constant buffer ranges per descriptor table parameter: kept from creation on (layouts are made at load
	// time, long before anyone listens), the descriptors themselves only while tracking
	struct TableRange
	{
		uint32_t	binding = 0;
		uint32_t	slot = 0;
		uint32_t	count = 0;
	};

	std::shared_mutex														layoutLock;
	std::unordered_map<uint64_t, std::vector<std::vector<TableRange>>>		layoutTables;

	[[ nodiscard ]] uint64_t descriptorKey ( api::device* device, api::descriptor_table table, uint32_t binding, uint32_t arrayOffset )
	{
		api::descriptor_heap	heap {};
		uint32_t				offset = 0;
		device->get_descriptor_heap_offset ( table, binding, arrayOffset, &heap, &offset );
		return heap.handle * 0x9E3779B97F4A7C15ull ^ offset;
	}

	struct Binding
	{
		api::shader_stage		stages {};
		api::pipeline_layout	layout {};
		uint32_t				param = 0;
		uint32_t				slot = 0;
		api::buffer_range		range {};
		std::vector<uint8_t>	constants;		// push constants: the values themselves
		uint64_t				version = 0;	// of the buffer when last handed over; 0 = not yet
		bool					isDirty = true;
	};

	struct __declspec ( uuid ( "6f0b6c2e-8a35-4f6e-9d2b-3c1a0cb00002" ) ) CommandListState
	{
		std::vector<Binding>	bindings;
	};

	bool	isTracking = false;

	[[ nodiscard ]] DeviceState& deviceState ( api::device* device )
	{
		if ( auto state = device->get_private_data<DeviceState> () )
			return *state;

		return *device->create_private_data<DeviceState> ();
	}

	[[ nodiscard ]] CommandListState& commandListState ( api::command_list* cmdList )
	{
		if ( auto state = cmdList->get_private_data<CommandListState> () )
			return *state;

		return *cmdList->create_private_data<CommandListState> ();
	}

	[[ nodiscard ]] bool keepsMapping ( api::device* device )
	{
		return device->get_api () == api::device_api::d3d12 || device->get_api () == api::device_api::vulkan;
	}

	[[ nodiscard ]] Binding& bindingFor ( CommandListState& state, api::shader_stage stages, api::pipeline_layout layout, uint32_t param, uint32_t slot )
	{
		for ( auto& binding : state.bindings )
			if ( binding.stages == stages && binding.layout == layout && binding.param == param && binding.slot == slot )
				return binding;

		auto&	binding = state.bindings.emplace_back ();
		binding.stages = stages;
		binding.layout = layout;
		binding.param = param;
		binding.slot = slot;
		return binding;
	}

	void storeBytes ( api::device* device, api::resource buffer, uint64_t offset, const void* data, uint64_t size )
	{
		auto&				state = deviceState ( device );
		const std::unique_lock	lock ( state.lock );
		auto&				contents = state.buffers[ buffer.handle ];

		if ( size == UINT64_MAX )
			size = device->get_resource_desc ( buffer ).buffer.size - offset;

		if ( contents.bytes.size () < offset + size )
			contents.bytes.resize ( size_t ( offset + size ) );

		std::memcpy ( contents.bytes.data () + offset, data, size_t ( size ) );
		++contents.version;
	}

	//-----------------------------------------------------------------------------

	void onMapBuffer ( api::device* device, api::resource buffer, uint64_t offset, uint64_t size, api::map_access access, void** data )
	{
		if ( data == nullptr || *data == nullptr || access == api::map_access::read_only )
			return;

		auto&				state = deviceState ( device );
		const std::unique_lock	lock ( state.lock );

		if ( keepsMapping ( device ) )
		{
			auto&	contents = state.buffers[ buffer.handle ];
			contents.mapped = static_cast<const uint8_t*> ( *data ) - offset;
			contents.mappedSize = device->get_resource_desc ( buffer ).buffer.size;
			++contents.version;
		}
		else
			state.pendingMaps[ buffer.handle ] = static_cast<uint8_t*> ( *data ) - offset;

		(void) size;
	}

	void onUnmapBuffer ( api::device* device, api::resource buffer )
	{
		auto&	state = deviceState ( device );
		void*	mapped = nullptr;

		{
			const std::unique_lock	lock ( state.lock );

			if ( const auto it = state.buffers.find ( buffer.handle ); it != state.buffers.end () )
				it->second.mapped = nullptr;

			if ( const auto it = state.pendingMaps.find ( buffer.handle ); it != state.pendingMaps.end () )
				mapped = it->second, state.pendingMaps.erase ( it );
		}

		// Constant buffers are small: the whole buffer as the game left it
		if ( mapped != nullptr )
			storeBytes ( device, buffer, 0, mapped, device->get_resource_desc ( buffer ).buffer.size );
	}

	bool onUpdateBuffer ( api::device* device, const void* data, api::resource buffer, uint64_t offset, uint64_t size )
	{
		if ( data != nullptr )
			storeBytes ( device, buffer, offset, data, size );

		return false;
	}

	bool onCopyBuffer ( api::command_list* cmdList, api::resource source, uint64_t sourceOffset, api::resource dest, uint64_t destOffset, uint64_t size )
	{
		const auto	device = cmdList->get_device ();
		auto&		state = deviceState ( device );
		std::vector<uint8_t>	bytes;

		{
			const std::shared_lock	lock ( state.lock );
			const auto				it = state.buffers.find ( source.handle );

			if ( it == state.buffers.end () )
				return false;

			const auto&	contents = it->second;
			const auto	length = size == UINT64_MAX ? device->get_resource_desc ( source ).buffer.size - sourceOffset : size;

			if ( contents.mapped != nullptr && sourceOffset + length <= contents.mappedSize )
				bytes.assign ( contents.mapped + sourceOffset, contents.mapped + sourceOffset + length );
			else if ( sourceOffset + length <= contents.bytes.size () )
				bytes.assign ( contents.bytes.begin () + ptrdiff_t ( sourceOffset ), contents.bytes.begin () + ptrdiff_t ( sourceOffset + length ) );
		}

		if ( ! bytes.empty () )
			storeBytes ( device, dest, destOffset, bytes.data (), bytes.size () );

		return false;
	}

	void onPushConstants ( api::command_list* cmdList, api::shader_stage stages, api::pipeline_layout layout, uint32_t param, uint32_t first, uint32_t count, const void* values )
	{
		auto&	binding = bindingFor ( commandListState ( cmdList ), stages, layout, param, 0 );

		if ( binding.constants.size () < size_t ( first + count ) * 4 )
			binding.constants.resize ( size_t ( first + count ) * 4 );

		std::memcpy ( binding.constants.data () + size_t ( first ) * 4, values, size_t ( count ) * 4 );
		binding.isDirty = true;
	}

	void onPushDescriptors ( api::command_list* cmdList, api::shader_stage stages, api::pipeline_layout layout, uint32_t param, const api::descriptor_table_update& update )
	{
		if ( update.type != api::descriptor_type::constant_buffer )
			return;

		auto&		state = commandListState ( cmdList );
		const auto	ranges = static_cast<const api::buffer_range*> ( update.descriptors );

		for ( uint32_t i = 0; i < update.count; ++i )
		{
			auto&	binding = bindingFor ( state, stages, layout, param, update.binding + update.array_offset + i );
			binding.range = ranges[ i ];
			binding.version = 0;
			binding.isDirty = true;
		}
	}

	void onInitPipelineLayout ( api::device*, uint32_t paramCount, const api::pipeline_layout_param* params, api::pipeline_layout layout )
	{
		std::vector<std::vector<TableRange>>	tables ( paramCount );
		auto									hasAny = false;

		for ( uint32_t p = 0; p < paramCount; ++p )
		{
			const auto&	param = params[ p ];
			const auto	isTable = param.type == api::pipeline_layout_param_type::descriptor_table;
			const auto	isTableWithFlags = param.type == api::pipeline_layout_param_type::descriptor_table_with_flags;

			if ( ! isTable && ! isTableWithFlags )
				continue;

			const auto	count = isTable ? param.descriptor_table.count : param.descriptor_table_with_flags.count;

			for ( uint32_t r = 0; r < count; ++r )
			{
				const api::descriptor_range&	range = isTable ? param.descriptor_table.ranges[ r ] : param.descriptor_table_with_flags.ranges[ r ];

				if ( range.type == api::descriptor_type::constant_buffer )
					tables[ p ].push_back ( { range.binding, range.dx_register_index, std::min ( range.count, 64u ) } ), hasAny = true;
			}
		}

		if ( hasAny )
		{
			const std::unique_lock	lock ( layoutLock );
			layoutTables[ layout.handle ] = std::move ( tables );
		}
	}

	void onDestroyPipelineLayout ( api::device*, api::pipeline_layout layout )
	{
		const std::unique_lock	lock ( layoutLock );
		layoutTables.erase ( layout.handle );
	}

	bool onUpdateDescriptorTables ( api::device* device, uint32_t count, const api::descriptor_table_update* updates )
	{
		auto&					state = deviceState ( device );
		const std::unique_lock	lock ( state.lock );

		for ( uint32_t u = 0; u < count; ++u )
		{
			const auto&	update = updates[ u ];

			for ( uint32_t k = 0; k < update.count; ++k )
			{
				const auto	key = descriptorKey ( device, update.table, update.binding, update.array_offset + k );

				if ( update.type == api::descriptor_type::constant_buffer )
					state.tableBuffers[ key ] = static_cast<const api::buffer_range*> ( update.descriptors )[ k ];
				else
					state.tableBuffers.erase ( key );
			}
		}

		return false;
	}

	bool onCopyDescriptorTables ( api::device* device, uint32_t count, const api::descriptor_table_copy* copies )
	{
		auto&					state = deviceState ( device );
		const std::unique_lock	lock ( state.lock );

		for ( uint32_t c = 0; c < count; ++c )
		{
			const auto&	copy = copies[ c ];

			for ( uint32_t k = 0; k < copy.count; ++k )
			{
				const auto	source = state.tableBuffers.find ( descriptorKey ( device, copy.source_table, copy.source_binding, copy.source_array_offset + k ) );
				const auto	dest = descriptorKey ( device, copy.dest_table, copy.dest_binding, copy.dest_array_offset + k );

				if ( source != state.tableBuffers.end () )
					state.tableBuffers[ dest ] = source->second;
				else
					state.tableBuffers.erase ( dest );
			}
		}

		return false;
	}

	void onBindDescriptorTables ( api::command_list* cmdList, api::shader_stage stages, api::pipeline_layout layout, uint32_t first, uint32_t count, const api::descriptor_table* tables,
								  uint32_t, const uint32_t* )
	{
		std::vector<std::vector<TableRange>>	ranges;

		{
			const std::shared_lock	lock ( layoutLock );
			const auto				it = layoutTables.find ( layout.handle );

			if ( it == layoutTables.end () )
				return;

			ranges = it->second;
		}

		const auto	device = cmdList->get_device ();
		auto&		buffers = deviceState ( device );
		auto&		state = commandListState ( cmdList );

		for ( uint32_t i = 0; i < count && first + i < ranges.size (); ++i )
			for ( const auto& range : ranges[ first + i ] )
				for ( uint32_t k = 0; k < range.count; ++k )
				{
					const auto	key = descriptorKey ( device, tables[ i ], range.binding, k );
					api::buffer_range	bound {};

					{
						const std::shared_lock	lock ( buffers.lock );

						if ( const auto it = buffers.tableBuffers.find ( key ); it != buffers.tableBuffers.end () )
							bound = it->second;
					}

					if ( bound.buffer == 0 )
						continue;

					auto&	binding = bindingFor ( state, stages, layout, first + i, range.slot + k );
					binding.range = bound;
					binding.version = 0;
					binding.isDirty = true;
				}
	}

	void onResetCommandList ( api::command_list* cmdList )
	{
		commandListState ( cmdList ).bindings.clear ();
	}

	// An upload buffer the game mapped before anyone listened: mapped once more (read-only, mappings are counted)
	void mapUploadBuffer ( api::device* device, DeviceState& state, api::resource buffer )
	{
		{
			const std::shared_lock	lock ( state.lock );

			if ( const auto it = state.buffers.find ( buffer.handle ); it != state.buffers.end () && ( it->second.mapped != nullptr || ! it->second.bytes.empty () ) )
				return;
		}

		const auto	desc = device->get_resource_desc ( buffer );
		void*		data = nullptr;

		if ( desc.heap != api::memory_heap::cpu_to_gpu || ! device->map_buffer_region ( buffer, 0, UINT64_MAX, api::map_access::read_only, &data ) || data == nullptr )
			return;

		const std::unique_lock	lock ( state.lock );
		auto&					contents = state.buffers[ buffer.handle ];
		contents.mapped = static_cast<const uint8_t*> ( data );
		contents.mappedSize = desc.buffer.size;
	}

	// Hands over every binding whose buffer changed since it was last handed over, or that was bound anew
	void handOver ( api::command_list* cmdList )
	{
		auto&		state = commandListState ( cmdList );
		const auto	device = cmdList->get_device ();
		auto&		buffers = deviceState ( device );
		std::vector<uint8_t>	bytes;

		for ( auto& binding : state.bindings )
		{
			if ( binding.range.buffer == 0 )
			{
				if ( binding.isDirty && ! binding.constants.empty () )
					invoke_addon_event<addon_event::ultra_constant_buffer> ( cmdList, binding.stages, binding.layout, binding.param, binding.slot, binding.constants.data (),
																			 uint64_t ( binding.constants.size () ), api::resource {}, uint64_t ( 0 ) );

				binding.isDirty = false;
				continue;
			}

			uint64_t	version = 0;
			auto		offset = binding.range.offset;
			auto		size = binding.range.size;
			bytes.clear ();

			if ( keepsMapping ( device ) )
				mapUploadBuffer ( device, buffers, binding.range.buffer );

			{
				const std::shared_lock	lock ( buffers.lock );
				const auto				it = buffers.buffers.find ( binding.range.buffer.handle );

				if ( it == buffers.buffers.end () )
					continue;

				const auto&	contents = it->second;
				const auto	available = contents.mapped != nullptr ? contents.mappedSize : uint64_t ( contents.bytes.size () );

				if ( offset >= available )
					continue;

				size = std::min ( size == UINT64_MAX ? available - offset : size, available - offset );

				// Persistently mapped memory may change without a call: always handed over
				version = contents.mapped != nullptr ? contents.version + 1 : contents.version;

				if ( ! binding.isDirty && version == binding.version )
					continue;

				const auto	source = contents.mapped != nullptr ? contents.mapped : contents.bytes.data ();
				bytes.assign ( source + offset, source + offset + size );
			}

			invoke_addon_event<addon_event::ultra_constant_buffer> ( cmdList, binding.stages, binding.layout, binding.param, binding.slot, bytes.data (), uint64_t ( bytes.size () ),
																	 binding.range.buffer, offset );
			binding.version = version;
			binding.isDirty = false;
		}
	}

	bool onDraw ( api::command_list* cmdList, uint32_t, uint32_t, uint32_t, uint32_t )
	{
		handOver ( cmdList );
		return false;
	}

	bool onDrawIndexed ( api::command_list* cmdList, uint32_t, uint32_t, uint32_t, int32_t, uint32_t )
	{
		handOver ( cmdList );
		return false;
	}

	bool onDispatch ( api::command_list* cmdList, uint32_t, uint32_t, uint32_t )
	{
		handOver ( cmdList );
		return false;
	}

	bool onIndirect ( api::command_list* cmdList, api::indirect_command, api::resource, uint64_t, uint32_t, uint32_t )
	{
		handOver ( cmdList );
		return false;
	}

	void onDestroyDevice ( api::device* device )
	{
		if ( device->get_private_data<DeviceState> () != nullptr )
			device->destroy_private_data<DeviceState> ();
	}

	void onDestroyCommandList ( api::command_list* cmdList )
	{
		if ( cmdList->get_private_data<CommandListState> () != nullptr )
			cmdList->destroy_private_data<CommandListState> ();
	}

	void onDestroyCommandQueue ( api::command_queue* queue )
	{
		if ( const auto immediate = queue->get_immediate_command_list () )
			onDestroyCommandList ( immediate );
	}

	void onDestroyResource ( api::device* device, api::resource resource )
	{
		if ( auto state = device->get_private_data<DeviceState> () )
		{
			const std::unique_lock	lock ( state->lock );
			state->buffers.erase ( resource.handle );
			state->pendingMaps.erase ( resource.handle );
		}
	}
}
//-----------------------------------------------------------------------------

// Only the cleanup is always registered: the tracking itself runs while someone listens (track_constant_buffers)
void register_addon_constant_buffers ()
{
	reshade::register_event<reshade::addon_event::destroy_device> ( onDestroyDevice );
	reshade::register_event<reshade::addon_event::destroy_command_list> ( onDestroyCommandList );
	reshade::register_event<reshade::addon_event::destroy_command_queue> ( onDestroyCommandQueue );
	reshade::register_event<reshade::addon_event::init_pipeline_layout> ( onInitPipelineLayout );
	reshade::register_event<reshade::addon_event::destroy_pipeline_layout> ( onDestroyPipelineLayout );
}

void unregister_addon_constant_buffers ()
{
	track_constant_buffers ( false );
	reshade::unregister_event<reshade::addon_event::init_pipeline_layout> ( onInitPipelineLayout );
	reshade::unregister_event<reshade::addon_event::destroy_pipeline_layout> ( onDestroyPipelineLayout );
	reshade::unregister_event<reshade::addon_event::destroy_device> ( onDestroyDevice );
	reshade::unregister_event<reshade::addon_event::destroy_command_list> ( onDestroyCommandList );
	reshade::unregister_event<reshade::addon_event::destroy_command_queue> ( onDestroyCommandQueue );
}

void track_constant_buffers ( bool shouldTrack )
{
	if ( shouldTrack == isTracking )
		return;

	isTracking = shouldTrack;

	if ( shouldTrack )
	{
		reshade::register_event<reshade::addon_event::map_buffer_region> ( onMapBuffer );
		reshade::register_event<reshade::addon_event::unmap_buffer_region> ( onUnmapBuffer );
		reshade::register_event<reshade::addon_event::update_buffer_region> ( onUpdateBuffer );
		reshade::register_event<reshade::addon_event::copy_buffer_region> ( onCopyBuffer );
		reshade::register_event<reshade::addon_event::push_constants> ( onPushConstants );
		reshade::register_event<reshade::addon_event::push_descriptors> ( onPushDescriptors );
		reshade::register_event<reshade::addon_event::update_descriptor_tables> ( onUpdateDescriptorTables );
		reshade::register_event<reshade::addon_event::copy_descriptor_tables> ( onCopyDescriptorTables );
		reshade::register_event<reshade::addon_event::bind_descriptor_tables> ( onBindDescriptorTables );
		reshade::register_event<reshade::addon_event::reset_command_list> ( onResetCommandList );
		reshade::register_event<reshade::addon_event::draw> ( onDraw );
		reshade::register_event<reshade::addon_event::draw_indexed> ( onDrawIndexed );
		reshade::register_event<reshade::addon_event::dispatch> ( onDispatch );
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect> ( onIndirect );
		reshade::register_event<reshade::addon_event::destroy_resource> ( onDestroyResource );
	}
	else
	{
		reshade::unregister_event<reshade::addon_event::map_buffer_region> ( onMapBuffer );
		reshade::unregister_event<reshade::addon_event::unmap_buffer_region> ( onUnmapBuffer );
		reshade::unregister_event<reshade::addon_event::update_buffer_region> ( onUpdateBuffer );
		reshade::unregister_event<reshade::addon_event::copy_buffer_region> ( onCopyBuffer );
		reshade::unregister_event<reshade::addon_event::push_constants> ( onPushConstants );
		reshade::unregister_event<reshade::addon_event::push_descriptors> ( onPushDescriptors );
		reshade::unregister_event<reshade::addon_event::update_descriptor_tables> ( onUpdateDescriptorTables );
		reshade::unregister_event<reshade::addon_event::copy_descriptor_tables> ( onCopyDescriptorTables );
		reshade::unregister_event<reshade::addon_event::bind_descriptor_tables> ( onBindDescriptorTables );
		reshade::unregister_event<reshade::addon_event::reset_command_list> ( onResetCommandList );
		reshade::unregister_event<reshade::addon_event::draw> ( onDraw );
		reshade::unregister_event<reshade::addon_event::draw_indexed> ( onDrawIndexed );
		reshade::unregister_event<reshade::addon_event::dispatch> ( onDispatch );
		reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect> ( onIndirect );
		reshade::unregister_event<reshade::addon_event::destroy_resource> ( onDestroyResource );
	}
}
