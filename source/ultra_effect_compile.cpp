/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

// ultra_effect_compile <out.fxpack> <effect.fx> [-I <folder>]... [-space 1,2,3] [-depth 8]: the binaries an effect needs on D3D11 and D3D12,
// built with the runtime's front end and under its names, as the .fxpack that ships beside the effect; every -space x -depth combination

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "effect_codegen.hpp"
#include "effect_front.hpp"
#include "effect_pack.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

namespace
{
	[[ nodiscard ]] std::string narrow ( const std::filesystem::path& path )
	{
		return path.u8string ();
	}

	// "1,2,3"; empty when it is not numbers
	[[ nodiscard ]] std::vector<uint32_t> numbers ( const std::wstring& text )
	{
		std::vector<uint32_t>	list;

		for ( size_t at = 0; at <= text.size (); )
		{
			auto	end = text.find ( L',', at );
			end = end == std::wstring::npos ? text.size () : end;

			wchar_t*	stop = nullptr;
			const auto	item = std::wcstoul ( text.c_str () + at, &stop, 10 );

			if ( end == at || stop != text.c_str () + end )
				return {};

			list.push_back ( static_cast<uint32_t> ( item ) );
			at = end + 1;
		}

		return list;
	}

	struct Job
	{
		std::filesystem::path				out;
		std::filesystem::path				effect;
		std::vector<std::filesystem::path>	includes;
		std::vector<uint32_t>				spaces { 1, 2, 3 };
		std::vector<uint32_t>				depths { 8 };
	};

	[[ nodiscard ]] bool isJobRead ( int argc, wchar_t** argv, Job& job )
	{
		if ( argc < 3 )
			return false;

		job.out = argv[ 1 ];
		job.effect = argv[ 2 ];

		for ( int i = 3; i < argc; i += 2 )
		{
			const std::wstring	option = argv[ i ];

			if ( i + 1 >= argc )
				return false;

			if ( option == L"-I" )
				job.includes.emplace_back ( argv[ i + 1 ] );
			else if ( option == L"-space" || option == L"-depth" )
			{
				auto&	list = option == L"-space" ? job.spaces : job.depths;
				list = numbers ( argv[ i + 1 ] );

				if ( list.empty () )
					return false;
			}
			else
				return false;
		}

		return true;
	}

	// The runtime's way for D3D11 and D3D12 (runtime.cpp load_effect): shader model 5.0, no debug information, optimization level 1
	[[ nodiscard ]] bool isBuilt ( const Job& job, uint32_t space, uint32_t depth, reshadefx::binary_pack& pack, unsigned& compiled )
	{
		reshadefx::preprocessor	pp;
		reshadefx::builtin_macros	macros;
		macros.renderer_id = 0xb000;
		macros.width = 1920;
		macros.height = 1080;
		macros.color_space = space;
		macros.color_bit_depth = depth;
		reshadefx::add_builtin_macros ( pp, macros );

		pp.add_include_path ( job.effect.parent_path () );
		for ( const auto& folder : job.includes )
			pp.add_include_path ( folder );
		reshadefx::add_conversion_macros ( pp );

		const auto	isPreprocessed = pp.append_file ( job.effect );
		auto		errors = pp.errors ();

		std::unique_ptr<reshadefx::codegen>	codegen ( reshadefx::create_codegen_dxbc ( 50, false, false, 1 ) );
		unsigned							found = 0;

		codegen->load_binary = [ &pack, &found ] ( const std::string& key, std::string& binary )
		{
			if ( ! pack.find ( key, binary ) )
				return false;
			++found;
			return true;
		};
		codegen->save_binary = [ &pack ] ( const std::string& key, const std::string& binary ) { pack.add ( key, binary ); };

		reshadefx::parser	parser;
		auto				isParsed = isPreprocessed && parser.parse ( pp.output (), codegen.get () );
		errors += parser.errors ();

		std::string	binary;
		for ( const auto& entry : codegen->module ().entry_points )
			isParsed = isParsed && codegen->assemble_code_for_entry_point ( entry.first, binary, errors );

		compiled += codegen->compile_count;

		if ( ! errors.empty () )
			std::fprintf ( stderr, "%s: space %u, bit depth %u:\n%s\n", narrow ( job.effect ).c_str (), space, depth, errors.c_str () );

		return isParsed;
	}
}

int wmain ( int argc, wchar_t** argv )
{
	Job	job;

	if ( ! isJobRead ( argc, argv, job ) )
	{
		std::fputs ( "usage: ultra_effect_compile <out.fxpack> <effect.fx> [-I <folder>]... [-space 1,2,3] [-depth 8]\n", stderr );
		return 2;
	}

	reshadefx::binary_pack	pack;
	unsigned				compiled = 0;

	for ( const auto space : job.spaces )
		for ( const auto depth : job.depths )
			if ( ! isBuilt ( job, space, depth, pack, compiled ) )
			{
				std::fprintf ( stderr, "%s does not build for space %u, bit depth %u.\n", narrow ( job.effect ).c_str (), space, depth );
				return 1;
			}

	std::string	why;

	if ( ! pack.write ( job.out, why ) )
	{
		std::fprintf ( stderr, "%s cannot be written: %s.\n", narrow ( job.out ).c_str (), why.c_str () );
		return 1;
	}

	std::printf ( "%s: %zu binaries (%u compiled) in %s\n", narrow ( job.effect.filename () ).c_str (), pack.size (), compiled, narrow ( job.out ).c_str () );
	return 0;
}
