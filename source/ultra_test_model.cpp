/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef RESHADE_TEST_APPLICATION

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

#include <Windows.h>
#include <shellapi.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <stb_image.h>
#include <ufbx.h>

#include "ultra_test_model.hpp"

//-----------------------------------------------------------------------------

namespace
{
	// What moves a vertex: the face's few animated joints and everything below them (none: neck and shoulders)
	enum RigGroup : uint8_t { none, head, jaw, eyeLeft, eyeRight, lidUpperLeft, lidUpperRight, lidLowerLeft, lidLowerRight, groupCount };
}

namespace reshade::ultra
{
	struct TestModelRig
	{
		std::vector<std::array<uint8_t, 4>>	groups;			// per model vertex, weights alongside
		std::vector<std::array<float, 4>>	weights;
		float								pivots[ groupCount ][ 3 ] {};
		float								forward = 1.0f;	// sign of the face's look direction along z
	};
}

namespace
{
	[[ nodiscard ]] std::vector<uint8_t> readFile ( const std::filesystem::path& path )
	{
		std::ifstream	file ( path, std::ios::binary );

		return { std::istreambuf_iterator<char> ( file ), std::istreambuf_iterator<char> () };
	}

	[[ nodiscard ]] bool loadImage ( const std::filesystem::path& path, reshade::ultra::TestModelImage& image )
	{
		const auto	bytes = readFile ( path );
		int			width = 0;
		int			height = 0;
		int			channels = 0;
		const auto	pixels = stbi_load_from_memory ( bytes.data (), static_cast<int> ( bytes.size () ), &width, &height, &channels, 4 );

		if ( pixels == nullptr )
			return false;

		image.width = static_cast<uint32_t> ( width );
		image.height = static_cast<uint32_t> ( height );
		image.rgba.assign ( pixels, pixels + size_t ( width ) * size_t ( height ) * 4 );
		stbi_image_free ( pixels );
		return true;
	}

	[[ nodiscard ]] bool readMesh ( const cgltf_data& data, reshade::ultra::TestModel& model )
	{
		if ( data.meshes_count == 0 || data.meshes[ 0 ].primitives_count == 0 )
			return false;

		const auto&		primitive = data.meshes[ 0 ].primitives[ 0 ];
		cgltf_accessor*	positions = nullptr;
		cgltf_accessor*	normals = nullptr;
		cgltf_accessor*	uvs = nullptr;

		for ( cgltf_size i = 0; i < primitive.attributes_count; ++i )
		{
			const auto&	attribute = primitive.attributes[ i ];

			if ( attribute.type == cgltf_attribute_type_position )
				positions = attribute.data;
			else if ( attribute.type == cgltf_attribute_type_normal )
				normals = attribute.data;
			else if ( attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0 )
				uvs = attribute.data;
		}

		if ( positions == nullptr || normals == nullptr || uvs == nullptr || primitive.indices == nullptr )
			return false;

		model.vertices.resize ( positions->count );

		for ( cgltf_size i = 0; i < positions->count; ++i )
		{
			auto&	vertex = model.vertices[ i ];

			if ( ! cgltf_accessor_read_float ( positions, i, vertex.position, 3 ) || ! cgltf_accessor_read_float ( normals, i, vertex.normal, 3 )
				 || ! cgltf_accessor_read_float ( uvs, i, vertex.uv, 2 ) )
				return false;

			// The scan's maps are stored upside down to its uvs (three.js loads them flipped)
			vertex.uv[ 1 ] = 1.0f - vertex.uv[ 1 ];
		}

		model.indices.resize ( primitive.indices->count );

		for ( cgltf_size i = 0; i < primitive.indices->count; ++i )
			model.indices[ i ] = static_cast<uint32_t> ( cgltf_accessor_read_index ( primitive.indices, i ) );

		float	lo[ 3 ] { 1e30f, 1e30f, 1e30f };
		float	hi[ 3 ] { -1e30f, -1e30f, -1e30f };

		for ( const auto& vertex : model.vertices )
			for ( auto axis = 0; axis < 3; ++axis )
			{
				lo[ axis ] = std::min ( lo[ axis ], vertex.position[ axis ] );
				hi[ axis ] = std::max ( hi[ axis ], vertex.position[ axis ] );
			}

		float	radiusSquared = 0.0f;

		for ( auto axis = 0; axis < 3; ++axis )
			model.center[ axis ] = 0.5f * ( lo[ axis ] + hi[ axis ] );

		for ( const auto& vertex : model.vertices )
		{
			const auto	dx = vertex.position[ 0 ] - model.center[ 0 ];
			const auto	dy = vertex.position[ 1 ] - model.center[ 1 ];
			const auto	dz = vertex.position[ 2 ] - model.center[ 2 ];

			radiusSquared = std::max ( radiusSquared, dx * dx + dy * dy + dz * dz );
		}

		model.radius = std::sqrt ( radiusSquared );
		return model.radius > 0.0f;
	}

	void fitBounds ( reshade::ultra::TestModel& model, uint32_t firstIndex, uint32_t indexCount )
	{
		float	lo[ 3 ] { 1e30f, 1e30f, 1e30f };
		float	hi[ 3 ] { -1e30f, -1e30f, -1e30f };

		for ( auto i = firstIndex; i < firstIndex + indexCount; ++i )
			for ( auto axis = 0; axis < 3; ++axis )
			{
				lo[ axis ] = std::min ( lo[ axis ], model.vertices[ model.indices[ i ] ].position[ axis ] );
				hi[ axis ] = std::max ( hi[ axis ], model.vertices[ model.indices[ i ] ].position[ axis ] );
			}

		float	radiusSquared = 0.0f;

		for ( auto axis = 0; axis < 3; ++axis )
			model.center[ axis ] = 0.5f * ( lo[ axis ] + hi[ axis ] );

		for ( auto i = firstIndex; i < firstIndex + indexCount; ++i )
		{
			const auto&	p = model.vertices[ model.indices[ i ] ].position;
			const auto	dx = p[ 0 ] - model.center[ 0 ];
			const auto	dy = p[ 1 ] - model.center[ 1 ];
			const auto	dz = p[ 2 ] - model.center[ 2 ];

			radiusSquared = std::max ( radiusSquared, dx * dx + dy * dy + dz * dz );
		}

		model.radius = std::sqrt ( radiusSquared );
	}

	// Unreal's normal maps point green down, the scan's (and every model here) green up
	void flipGreen ( reshade::ultra::TestModelImage& image )
	{
		for ( size_t i = 1; i < image.rgba.size (); i += 4 )
			image.rgba[ i ] = static_cast<uint8_t> ( 255 - image.rgba[ i ] );
	}

	[[ nodiscard ]] std::optional<std::filesystem::path> findFile ( const std::filesystem::path& folder, const std::wstring& suffix )
	{
		std::error_code	ec;

		for ( const auto& entry : std::filesystem::recursive_directory_iterator ( folder, ec ) )
		{
			const auto	name = entry.path ().filename ().wstring ();

			if ( entry.is_regular_file () && name.size () >= suffix.size () && _wcsicmp ( name.c_str () + name.size () - suffix.size (), suffix.c_str () ) == 0 )
				return entry.path ();
		}

		return std::nullopt;
	}

	// The head mesh's sections, in MetaHuman's order; saliva (2) and cartilage (8) are skipped
	enum class HeadSection { skin = 0, teeth = 1, eyeLeft = 3, eyeRight = 4, eyeShell = 5, eyelashes = 6, eyeEdge = 7 };

	struct SectionMaps
	{
		HeadSection						section;
		reshade::ultra::TestPartKind	kind;
		const wchar_t*					colour;
		const wchar_t*					normal;
		const wchar_t*					specular;
	};

	// Opaque parts first, the eye shell (occlusion) and eye edge (tear line) last: they blend over the eyes
	constexpr SectionMaps	sectionMaps[] = {
		{ HeadSection::skin, reshade::ultra::TestPartKind::skin, L"Head_Basecolor.png", L"Head_Normal.png", L"Head_SRMF.png" },
		{ HeadSection::teeth, reshade::ultra::TestPartKind::teeth, L"Teeth_Color.png", L"Teeth_Normal.png", nullptr },
		{ HeadSection::eyeLeft, reshade::ultra::TestPartKind::eye, L"Eyes_Color.png", L"Eyes_Normal.png", nullptr },
		{ HeadSection::eyeRight, reshade::ultra::TestPartKind::eye, L"Eyes_Color.png", L"Eyes_Normal.png", nullptr },
		{ HeadSection::eyelashes, reshade::ultra::TestPartKind::eyelashes, L"Eyelashes_Color.png", nullptr, nullptr },
		{ HeadSection::eyeShell, reshade::ultra::TestPartKind::eyeOcclusion, nullptr, nullptr, nullptr },
		{ HeadSection::eyeEdge, reshade::ultra::TestPartKind::tearLine, nullptr, nullptr, nullptr } };

	[[ nodiscard ]] ufbx_scene* loadFbx ( const std::filesystem::path& path )
	{
		ufbx_load_opts	options {};
		options.target_axes = ufbx_axes_right_handed_y_up;
		options.target_unit_meters = 1.0f;
		options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;

		const auto	file = path.u8string ();
		ufbx_error	error {};
		return ufbx_load_file_len ( reinterpret_cast<const char*> ( file.data () ), file.size (), &options, &error );
	}

	// The number after "key": in the material parameter dump; vectors are read component by component
	[[ nodiscard ]] float jsonNumber ( const std::string& json, const char* key, size_t component = 0, float fallback = 0.0f )
	{
		auto	at = json.find ( std::string ( "\"" ) + key + "\":" );

		if ( at == std::string::npos )
			return fallback;

		at = json.find_first_not_of ( " \t\r\n", json.find ( ':', at ) + 1 );

		if ( at != std::string::npos && json[ at ] == '[' )
			for ( size_t k = 0; k < component && at != std::string::npos; ++k )
				at = json.find ( ',', at + 1 );

		if ( at == std::string::npos )
			return fallback;

		return std::strtof ( json.c_str () + at + ( json[ at ] == '[' || json[ at ] == ',' ? 1 : 0 ), nullptr );
	}

	[[ nodiscard ]] reshade::ultra::TestHairColour readHairColour ( const std::filesystem::path& parameters )
	{
		const auto					bytes = readFile ( parameters );
		const std::string			json ( bytes.begin (), bytes.end () );
		reshade::ultra::TestHairColour	colour;

		colour.melanin = jsonNumber ( json, "hairMelanin" );
		colour.redness = jsonNumber ( json, "hairRedness" );
		colour.ombre = jsonNumber ( json, "Ombre" ) * jsonNumber ( json, "OmbreIntensity", 0, 1.0f );
		colour.ombreShift = jsonNumber ( json, "OmbreShift" );
		colour.ombreMelanin = jsonNumber ( json, "OmbreMelanin" );
		colour.ombreRedness = jsonNumber ( json, "OmbreRedness" );
		colour.highlights = jsonNumber ( json, "Highlights" );
		colour.highlightsIntensity = jsonNumber ( json, "HighlightsIntensity" );
		colour.highlightsMelanin = jsonNumber ( json, "HighlightsMelanin" );
		colour.highlightsRedness = jsonNumber ( json, "HighlightsRedness" );
		colour.roughness = jsonNumber ( json, "Roughness", 0, 0.5f );
		colour.specular = jsonNumber ( json, "Spec0", 0, 0.5f );

		for ( size_t c = 0; c < 3; ++c )
		{
			colour.dye[ c ] = jsonNumber ( json, "hairDye", c, 1.0f );
			colour.ombreDye[ c ] = jsonNumber ( json, "OmbrehairDye", c, 1.0f );
			colour.highlightsDye[ c ] = jsonNumber ( json, "HighlightshairDye", c, 1.0f );
		}

		return colour;
	}

	// A flat tangent per triangle along +u of the uvs as stored (after the v flip), w from the uv winding
	void addTriangleTangents ( reshade::ultra::TestModel& model, size_t firstVertex )
	{
		for ( auto v = firstVertex; v + 2 < model.vertices.size (); v += 3 )
		{
			const auto&	a = model.vertices[ v ];
			const auto&	b = model.vertices[ v + 1 ];
			const auto&	c = model.vertices[ v + 2 ];
			float		e1[ 3 ];
			float		e2[ 3 ];

			for ( auto i = 0; i < 3; ++i )
				e1[ i ] = b.position[ i ] - a.position[ i ], e2[ i ] = c.position[ i ] - a.position[ i ];

			const auto	du1 = b.uv[ 0 ] - a.uv[ 0 ];
			const auto	dv1 = b.uv[ 1 ] - a.uv[ 1 ];
			const auto	du2 = c.uv[ 0 ] - a.uv[ 0 ];
			const auto	dv2 = c.uv[ 1 ] - a.uv[ 1 ];
			const auto	det = du1 * dv2 - du2 * dv1;
			const auto	r = std::abs ( det ) > 1e-12f ? 1.0f / det : 0.0f;
			float		t[ 3 ];

			for ( auto i = 0; i < 3; ++i )
				t[ i ] = ( e1[ i ] * dv2 - e2[ i ] * dv1 ) * r;

			const auto	length = std::sqrt ( t[ 0 ] * t[ 0 ] + t[ 1 ] * t[ 1 ] + t[ 2 ] * t[ 2 ] );
			const auto	handedness = det < 0.0f ? -1.0f : 1.0f;

			for ( auto k = 0; k < 3; ++k )
				model.tangents[ v + k ] = length > 0.0f ? std::array<float, 4> { t[ 0 ] / length, t[ 1 ] / length, t[ 2 ] / length, handedness } : std::array<float, 4> {};
		}
	}

	[[ nodiscard ]] const ufbx_mesh* finestHeadMesh ( const ufbx_scene& scene )
	{
		const ufbx_mesh*	finest = nullptr;

		for ( size_t i = 0; i < scene.meshes.count; ++i )
			if ( finest == nullptr || scene.meshes.data[ i ]->num_triangles > finest->num_triangles )
				finest = scene.meshes.data[ i ];

		return finest;
	}

	struct VertexGroups
	{
		std::array<uint8_t, 4>	groups {};
		std::array<float, 4>	weights {};
	};

	[[ nodiscard ]] RigGroup groupOfJoint ( const ufbx_node* joint )
	{
		for ( ; joint != nullptr; joint = joint->parent )
		{
			const std::string_view	name ( joint->name.data, joint->name.length );

			if ( name == "FACIAL_L_EyelidUpperA" || name == "FACIAL_L_EyelidUpperB" )
				return lidUpperLeft;
			if ( name == "FACIAL_R_EyelidUpperA" || name == "FACIAL_R_EyelidUpperB" )
				return lidUpperRight;
			if ( name == "FACIAL_L_EyelidLowerA" || name == "FACIAL_L_EyelidLowerB" )
				return lidLowerLeft;
			if ( name == "FACIAL_R_EyelidLowerA" || name == "FACIAL_R_EyelidLowerB" )
				return lidLowerRight;
			if ( name == "FACIAL_L_Eye" )
				return eyeLeft;
			if ( name == "FACIAL_R_Eye" )
				return eyeRight;
			if ( name == "FACIAL_C_Jaw" || name.find ( "TeethLower" ) != std::string_view::npos || name.find ( "Tongue" ) != std::string_view::npos )
				return jaw;
			if ( name == "head" )
				return head;
		}

		return none;
	}

	// Each mesh vertex's skin weights summed per driver group, the four largest kept
	[[ nodiscard ]] std::vector<VertexGroups> skinGroups ( const ufbx_mesh& mesh )
	{
		std::vector<VertexGroups>	result ( mesh.num_vertices, VertexGroups { { head }, { 1.0f } } );

		if ( mesh.skin_deformers.count == 0 )
			return result;

		const auto&				skin = *mesh.skin_deformers.data[ 0 ];
		std::vector<RigGroup>	clusterGroups;

		for ( size_t c = 0; c < skin.clusters.count; ++c )
			clusterGroups.push_back ( groupOfJoint ( skin.clusters.data[ c ]->bone_node ) );

		for ( size_t v = 0; v < mesh.num_vertices && v < skin.vertices.count; ++v )
		{
			float		sums[ groupCount ] {};
			const auto&	weights = skin.vertices.data[ v ];

			for ( uint32_t w = 0; w < weights.num_weights; ++w )
			{
				const auto&	weight = skin.weights.data[ weights.weight_begin + w ];
				sums[ clusterGroups[ weight.cluster_index ] ] += float ( weight.weight );
			}

			VertexGroups	kept;
			float			total = 0.0f;

			for ( auto k = 0; k < 4; ++k )
			{
				const auto	best = std::max_element ( std::begin ( sums ), std::end ( sums ) ) - std::begin ( sums );
				kept.groups[ k ] = static_cast<uint8_t> ( best );
				kept.weights[ k ] = sums[ best ];
				total += sums[ best ];
				sums[ best ] = 0.0f;
			}

			for ( auto& w : kept.weights )
				w = total > 0.0f ? w / total : 0.0f;

			result[ v ] = kept;
		}

		return result;
	}

	[[ nodiscard ]] std::array<float, 3> jointPosition ( const ufbx_scene& scene, const char* name )
	{
		const auto	joint = ufbx_find_node ( &scene, name );

		if ( joint == nullptr )
			return {};

		const auto	p = joint->node_to_world.cols[ 3 ];
		return { float ( p.x ), float ( p.y ), float ( p.z ) };
	}

	// One vertex per triangle corner, no sharing: a few MB for the finest MetaHuman head. With a rig, each corner
	// gets its mesh vertex's driver groups, or rides the head when the mesh has none (hair and eyebrow cards)
	void appendSection ( const ufbx_mesh& mesh, const ufbx_node& node, size_t partIndex, reshade::ultra::TestModel& model, reshade::ultra::TestModelRig* rig = nullptr,
						 const std::vector<VertexGroups>* groups = nullptr )
	{
		const auto					normalMatrix = ufbx_matrix_for_normals ( &node.geometry_to_world );
		const auto&					part = mesh.material_parts.data[ partIndex ];
		std::vector<uint32_t>		corners ( mesh.max_face_triangles * 3 );

		for ( size_t f = 0; f < part.num_faces; ++f )
		{
			const auto	face = mesh.faces.data[ part.face_indices.data[ f ] ];
			const auto	triangles = ufbx_triangulate_face ( corners.data (), corners.size (), &mesh, face );

			for ( uint32_t c = 0; c < triangles * 3; ++c )
			{
				const auto		index = corners[ c ];
				const auto		p = ufbx_transform_position ( &node.geometry_to_world, ufbx_get_vertex_vec3 ( &mesh.vertex_position, index ) );
				const auto		n = ufbx_vec3_normalize ( ufbx_transform_direction ( &normalMatrix, ufbx_get_vertex_vec3 ( &mesh.vertex_normal, index ) ) );
				const auto		uv = ufbx_get_vertex_vec2 ( &mesh.vertex_uv, index );

				model.indices.push_back ( static_cast<uint32_t> ( model.vertices.size () ) );
				model.vertices.push_back ( { { float ( p.x ), float ( p.y ), float ( p.z ) }, { float ( n.x ), float ( n.y ), float ( n.z ) }, { float ( uv.x ), float ( 1.0 - uv.y ) } } );
				model.tangents.push_back ( {} );

				if ( rig != nullptr )
				{
					const auto	vertex = groups != nullptr ? ( *groups )[ mesh.vertex_indices.data[ index ] ] : VertexGroups { { head }, { 1.0f } };
					rig->groups.push_back ( vertex.groups );
					rig->weights.push_back ( vertex.weights );
				}
			}
		}
	}

	// The bottom of the neck maps to an unused part of the head texture (the body covers it in Unreal): triangles in
	// its lowest few centimetres are left out of the part's indices
	void dropNeckBase ( reshade::ultra::TestModel& model, uint32_t firstIndex )
	{
		constexpr float	band = 0.04f;
		auto			lowest = 1e30f;

		for ( auto i = firstIndex; i < model.indices.size (); ++i )
			lowest = std::min ( lowest, model.vertices[ model.indices[ i ] ].position[ 1 ] );

		const auto	isNeck = [ & ] ( uint32_t vertex ) { return model.vertices[ vertex ].position[ 1 ] < lowest + band; };

		std::vector<uint32_t>	kept;

		for ( auto i = firstIndex; i + 2 < model.indices.size (); i += 3 )
			if ( ! ( isNeck ( model.indices[ i ] ) && isNeck ( model.indices[ i + 1 ] ) && isNeck ( model.indices[ i + 2 ] ) ) )
				kept.insert ( kept.end (), { model.indices[ i ], model.indices[ i + 1 ], model.indices[ i + 2 ] } );

		model.indices.resize ( firstIndex );
		model.indices.insert ( model.indices.end (), kept.begin (), kept.end () );
	}

	// A groom's cards (every group of hair, or the eyebrows) as one part with tangents
	[[ nodiscard ]] bool appendCards ( const std::filesystem::path& grooms, const std::wstring& groom, reshade::ultra::TestPartKind kind, reshade::ultra::TestModel& model,
									   reshade::ultra::TestModelRig& rig )
	{
		std::error_code	ec;
		reshade::ultra::TestModelPart	part;
		part.kind = kind;
		part.firstIndex = static_cast<uint32_t> ( model.indices.size () );

		const auto	firstVertex = model.vertices.size ();

		for ( const auto& entry : std::filesystem::directory_iterator ( grooms, ec ) )
		{
			const auto	name = entry.path ().filename ().wstring ();

			if ( name.rfind ( groom + L"_CardsMesh_Group", 0 ) != 0 || name.find ( L"_LOD0." ) == std::wstring::npos || _wcsicmp ( entry.path ().extension ().c_str (), L".fbx" ) != 0 )
				continue;

			const auto	scene = loadFbx ( entry.path () );

			if ( scene == nullptr )
				return false;

			for ( size_t m = 0; m < scene->meshes.count; ++m )
			{
				const auto&	mesh = *scene->meshes.data[ m ];

				if ( mesh.instances.count > 0 && mesh.vertex_normal.exists && mesh.vertex_uv.exists )
					for ( size_t p = 0; p < mesh.material_parts.count; ++p )
						appendSection ( mesh, *mesh.instances.data[ 0 ], p, model, &rig );
			}

			ufbx_free_scene ( scene );
		}

		part.indexCount = static_cast<uint32_t> ( model.indices.size () ) - part.firstIndex;
		addTriangleTangents ( model, firstVertex );

		if ( part.indexCount == 0 || ! loadImage ( grooms / ( groom + L"_CardsAtlas_Attribute.png" ), part.colour )
			 || ! loadImage ( grooms / ( groom + L"_CardsAtlas_Tangent.png" ), part.normalMap ) )
			return false;

		(void) loadImage ( grooms / L"HighlightMask_strips.png", part.specular );
		part.hairColour = readHairColour ( grooms / ( L"MI_WI_" + groom + ( kind == reshade::ultra::TestPartKind::hair ? L"_Hair_Cards.json" : L"_Hair.json" ) ) );
		model.parts.push_back ( std::move ( part ) );
		return true;
	}

	// The groom's name from its card mesh file: <name>_CardsMesh_Group0_LOD0.fbx
	[[ nodiscard ]] std::optional<std::wstring> groomName ( const std::filesystem::path& grooms, const wchar_t* prefix )
	{
		std::error_code	ec;

		for ( const auto& entry : std::filesystem::directory_iterator ( grooms, ec ) )
		{
			const auto	name = entry.path ().filename ().wstring ();
			const auto	cards = name.find ( L"_CardsMesh_Group0_LOD0" );

			if ( name.rfind ( prefix, 0 ) == 0 && cards != std::wstring::npos )
				return name.substr ( 0, cards );
		}

		return std::nullopt;
	}
}
//-----------------------------------------------------------------------------

std::optional<reshade::ultra::TestModel> reshade::ultra::loadTestHead ( const std::filesystem::path& folder )
{
	const auto		glb = readFile ( folder / L"LeePerrySmith.glb" );
	cgltf_options	options {};
	cgltf_data*		data = nullptr;

	if ( glb.empty () || cgltf_parse ( &options, glb.data (), glb.size (), &data ) != cgltf_result_success )
		return std::nullopt;

	TestModel	model;
	const auto	meshRead = cgltf_load_buffers ( &options, data, nullptr ) == cgltf_result_success && readMesh ( *data, model );

	cgltf_free ( data );

	if ( ! meshRead || ! loadImage ( folder / L"Map-COL.jpg", model.colour ) || ! loadImage ( folder / L"Infinite-Level_02_Tangent_SmoothUV.jpg", model.normalMap )
		 || ! loadImage ( folder / L"Map-SPEC.jpg", model.specular ) )
		return std::nullopt;

	return model;
}
//-----------------------------------------------------------------------------

std::optional<std::filesystem::path> reshade::ultra::testModelFolder ()
{
	auto					argc = 0;
	const auto				argv = CommandLineToArgvW ( GetCommandLineW (), &argc );
	std::optional<std::filesystem::path>	folder;

	for ( auto i = 1; argv != nullptr && i + 1 < argc; ++i )
		if ( _wcsicmp ( argv[ i ], L"-model" ) == 0 )
			folder = std::filesystem::path ( argv[ i + 1 ] );

	LocalFree ( argv );
	return folder;
}
//-----------------------------------------------------------------------------

// Metres, Y up, uv origin top left and normal maps green up, as the scan
std::optional<reshade::ultra::TestModel> reshade::ultra::loadMetaHumanHead ( const std::filesystem::path& folder )
{
	const auto	fbxPath = findFile ( folder, L"_Head.fbx" );
	const auto	mapsPath = findFile ( folder, L"Head_Basecolor.png" );

	if ( ! fbxPath.has_value () || ! mapsPath.has_value () )
		return std::nullopt;

	const auto	scene = loadFbx ( *fbxPath );

	if ( scene == nullptr )
		return std::nullopt;

	TestModel	model;
	const auto	mesh = finestHeadMesh ( *scene );
	const auto	maps = mapsPath->parent_path ();
	auto		isLoaded = mesh != nullptr && mesh->instances.count > 0 && mesh->vertex_normal.exists && mesh->vertex_uv.exists;
	auto		rig = std::make_shared<TestModelRig> ();
	const auto	groups = isLoaded ? skinGroups ( *mesh ) : std::vector<VertexGroups> {};

	rig->forward = jointPosition ( *scene, "FACIAL_L_Eye" )[ 2 ] >= jointPosition ( *scene, "head" )[ 2 ] ? 1.0f : -1.0f;

	for ( const auto& [ group, joint ] : { std::pair { head, "head" }, { jaw, "FACIAL_C_Jaw" }, { eyeLeft, "FACIAL_L_Eye" }, { eyeRight, "FACIAL_R_Eye" },
										   { lidUpperLeft, "FACIAL_L_Eye" }, { lidUpperRight, "FACIAL_R_Eye" }, { lidLowerLeft, "FACIAL_L_Eye" }, { lidLowerRight, "FACIAL_R_Eye" } } )
	{
		const auto	p = jointPosition ( *scene, joint );
		std::copy ( p.begin (), p.end (), rig->pivots[ group ] );
	}

	for ( const auto& entry : sectionMaps )
	{
		const auto	partIndex = static_cast<size_t> ( entry.section );

		if ( ! isLoaded || partIndex >= mesh->material_parts.count )
			break;

		TestModelPart	part;
		part.firstIndex = static_cast<uint32_t> ( model.indices.size () );
		appendSection ( *mesh, *mesh->instances.data[ 0 ], partIndex, model, rig.get (), &groups );

		if ( entry.kind == TestPartKind::skin )
			dropNeckBase ( model, part.firstIndex );

		part.indexCount = static_cast<uint32_t> ( model.indices.size () ) - part.firstIndex;
		part.alphaTest = entry.kind == TestPartKind::eyelashes;
		part.kind = entry.kind;

		isLoaded = ( entry.colour == nullptr || loadImage ( maps / entry.colour, part.colour ) ) && ( entry.normal == nullptr || loadImage ( maps / entry.normal, part.normalMap ) )
				   && ( entry.specular == nullptr || loadImage ( maps / entry.specular, part.specular ) );
		flipGreen ( part.normalMap );

		// Wet eyes and teeth: a plain specular level where the export has no map (red, as the skin's SRMF)
		if ( entry.kind == TestPartKind::eye || entry.kind == TestPartKind::teeth )
			part.specular = { 1, 1, { static_cast<uint8_t> ( entry.kind == TestPartKind::teeth ? 140 : 230 ), 40, 0, 255 } };

		if ( entry.section == HeadSection::skin )
			fitBounds ( model, part.firstIndex, part.indexCount );

		model.parts.push_back ( std::move ( part ) );
	}

	ufbx_free_scene ( scene );

	if ( ! isLoaded || model.parts.size () != std::size ( sectionMaps ) || model.radius <= 0.0f )
		return std::nullopt;

	// Hair and eyebrows when the grooms were exported too; a head without them still loads
	const auto	grooms = folder / L"grooms";

	if ( const auto hair = groomName ( grooms, L"Hair_" ); hair.has_value () && ! appendCards ( grooms, *hair, TestPartKind::hair, model, *rig ) )
		return std::nullopt;

	if ( const auto eyebrows = groomName ( grooms, L"Eyebrows_" ); eyebrows.has_value () && ! appendCards ( grooms, *eyebrows, TestPartKind::eyebrows, model, *rig ) )
		return std::nullopt;

	model.rig = std::move ( rig );
	return model;
}
//-----------------------------------------------------------------------------

namespace
{
	// Row-major 3x4: rotation in the first three columns, translation in the fourth
	struct Affine
	{
		float	m[ 3 ][ 4 ] { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

		[[ nodiscard ]] Affine operator* ( const Affine& b ) const
		{
			Affine	r;

			for ( auto i = 0; i < 3; ++i )
				for ( auto j = 0; j < 4; ++j )
					r.m[ i ][ j ] = m[ i ][ 0 ] * b.m[ 0 ][ j ] + m[ i ][ 1 ] * b.m[ 1 ][ j ] + m[ i ][ 2 ] * b.m[ 2 ][ j ] + ( j == 3 ? m[ i ][ 3 ] : 0.0f );

			return r;
		}
	};

	// Rotation by pitch about x, then yaw about y, around a pivot
	[[ nodiscard ]] Affine rotationAround ( const float ( &pivot )[ 3 ], float pitch, float yaw )
	{
		const auto	cp = std::cos ( pitch );
		const auto	sp = std::sin ( pitch );
		const auto	cy = std::cos ( yaw );
		const auto	sy = std::sin ( yaw );
		const float	r[ 3 ][ 3 ] = { { cy, sy * sp, sy * cp }, { 0, cp, -sp }, { -sy, cy * sp, cy * cp } };
		Affine		a;

		for ( auto i = 0; i < 3; ++i )
		{
			for ( auto j = 0; j < 3; ++j )
				a.m[ i ][ j ] = r[ i ][ j ];

			a.m[ i ][ 3 ] = pivot[ i ] - ( r[ i ][ 0 ] * pivot[ 0 ] + r[ i ][ 1 ] * pivot[ 1 ] + r[ i ][ 2 ] * pivot[ 2 ] );
		}

		return a;
	}

	[[ nodiscard ]] float smoothstep ( float edge0, float edge1, float x )
	{
		const auto	t = std::clamp ( ( x - edge0 ) / ( edge1 - edge0 ), 0.0f, 1.0f );
		return t * t * ( 3.0f - 2.0f * t );
	}

	// 0 open, 1 closed: a quick close, a short hold, a slower open, every few seconds at uneven intervals
	[[ nodiscard ]] float blink ( float seconds )
	{
		constexpr float	starts[] = { 0.6f, 3.9f, 6.2f, 6.55f, 9.8f };
		constexpr float	cycle = 11.0f;
		const auto		t = std::fmod ( seconds, cycle );
		auto			closed = 0.0f;

		for ( const auto start : starts )
		{
			const auto	local = t - start;
			closed = std::max ( closed, smoothstep ( 0.0f, 0.07f, local ) * ( 1.0f - smoothstep ( 0.1f, 0.26f, local ) ) );
		}

		return closed;
	}

	// Gaze in radians (pitch, yaw): fixations held for a while, quick saccades between them
	[[ nodiscard ]] std::array<float, 2> gaze ( float seconds )
	{
		constexpr float	targets[][ 2 ] = { { 0.0f, 0.0f }, { 0.04f, 0.18f }, { -0.05f, 0.1f }, { 0.0f, -0.2f }, { 0.08f, -0.06f }, { -0.03f, 0.22f } };
		constexpr float	hold = 1.4f;
		const auto		count = std::size ( targets );
		const auto		step = static_cast<size_t> ( std::floor ( seconds / hold ) );
		const auto		blend = smoothstep ( 0.0f, 0.045f, std::fmod ( seconds, hold ) );
		const auto&		from = targets[ ( step + count - 1 ) % count ];
		const auto&		to = targets[ step % count ];

		return { from[ 0 ] + ( to[ 0 ] - from[ 0 ] ) * blend, from[ 1 ] + ( to[ 1 ] - from[ 1 ] ) * blend };
	}
}
//-----------------------------------------------------------------------------

// Every rotation is about the x (sideways) and y (up) axes: the face looks along z (TestModelRig::forward)
bool reshade::ultra::poseTestModel ( const TestModel& model, float seconds, std::vector<TestModelVertex>& vertices )
{
	if ( model.rig == nullptr || model.rig->groups.size () != model.vertices.size () )
		return false;

	const auto&	rig = *model.rig;
	const auto	down = rig.forward;				// positive pitch turns the front of the face down
	const auto	closed = blink ( seconds );
	const auto	look = gaze ( seconds );
	Affine		matrices[ groupCount ];

	// The jaw rides the head only: its joints alone open the chin without the lips following
	matrices[ head ] = rotationAround ( rig.pivots[ head ], down * 0.06f * std::sin ( seconds * 1.46f ), 0.14f * std::sin ( seconds * 1.05f ) );
	matrices[ jaw ] = matrices[ head ];
	matrices[ eyeLeft ] = matrices[ head ] * rotationAround ( rig.pivots[ eyeLeft ], down * look[ 0 ], look[ 1 ] );
	matrices[ eyeRight ] = matrices[ head ] * rotationAround ( rig.pivots[ eyeRight ], down * look[ 0 ], look[ 1 ] );
	matrices[ lidUpperLeft ] = matrices[ head ] * rotationAround ( rig.pivots[ lidUpperLeft ], down * ( 0.45f * closed + 0.6f * look[ 0 ] ), 0.0f );
	matrices[ lidUpperRight ] = matrices[ head ] * rotationAround ( rig.pivots[ lidUpperRight ], down * ( 0.45f * closed + 0.6f * look[ 0 ] ), 0.0f );
	matrices[ lidLowerLeft ] = matrices[ head ] * rotationAround ( rig.pivots[ lidLowerLeft ], -down * 0.12f * closed, 0.0f );
	matrices[ lidLowerRight ] = matrices[ head ] * rotationAround ( rig.pivots[ lidLowerRight ], -down * 0.12f * closed, 0.0f );

	vertices.resize ( model.vertices.size () );

	for ( size_t v = 0; v < model.vertices.size (); ++v )
	{
		const auto&	rest = model.vertices[ v ];
		auto&		out = vertices[ v ];
		float		p[ 3 ] {};
		float		n[ 3 ] {};
		float		total = 0.0f;

		for ( auto k = 0; k < 4; ++k )
		{
			const auto	w = rig.weights[ v ][ k ];

			if ( w <= 0.0f )
				continue;

			total += w;

			const auto&	m = matrices[ rig.groups[ v ][ k ] ].m;

			for ( auto i = 0; i < 3; ++i )
			{
				p[ i ] += w * ( m[ i ][ 0 ] * rest.position[ 0 ] + m[ i ][ 1 ] * rest.position[ 1 ] + m[ i ][ 2 ] * rest.position[ 2 ] + m[ i ][ 3 ] );
				n[ i ] += w * ( m[ i ][ 0 ] * rest.normal[ 0 ] + m[ i ][ 1 ] * rest.normal[ 1 ] + m[ i ][ 2 ] * rest.normal[ 2 ] );
			}
		}

		const auto	length = std::sqrt ( n[ 0 ] * n[ 0 ] + n[ 1 ] * n[ 1 ] + n[ 2 ] * n[ 2 ] );

		for ( auto i = 0; i < 3; ++i )
		{
			out.position[ i ] = total > 0.0f ? p[ i ] : rest.position[ i ];
			out.normal[ i ] = length > 0.0f ? n[ i ] / length : rest.normal[ i ];
		}

		out.uv[ 0 ] = rest.uv[ 0 ];
		out.uv[ 1 ] = rest.uv[ 1 ];
	}

	return true;
}
//-----------------------------------------------------------------------------

#endif
