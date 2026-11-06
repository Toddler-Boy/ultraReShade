/*
 * Copyright (C) 2026 Michael Hartmann
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

//-----------------------------------------------------------------------------

// The test application's head (res/test_scene/head, the Infinite head scan by Lee Perry-Smith, CC BY 3.0), on the CPU
namespace reshade::ultra
{
	struct TestModelVertex
	{
		float	position[ 3 ];
		float	normal[ 3 ];
		float	uv[ 2 ];
	};

	struct TestModelImage
	{
		uint32_t				width = 0;
		uint32_t				height = 0;
		std::vector<uint8_t>	rgba;
	};

	// What a part's images hold and how it is drawn
	//   surface, skin, teeth, eye: colour, normalMap (green up), specular (r level as Unreal's Specular, g roughness)
	//   eyelashes: colour with coverage in a (alpha test)
	//   eyeOcclusion, tearLine: no images, the shape comes from the uvs (Unreal draws both procedurally, translucent)
	//   hair, eyebrows: Unreal's card atlases, layout 2: colour = attribute atlas (r coverage, g depth, b seed),
	//   normalMap = tangent atlas (rgb card-space tangent, a coordinate along the strand from root 0 to tip 1),
	//   specular = highlight strips mask (r); colours from hairColour; per-vertex tangents in TestModel::tangents
	enum class TestPartKind { surface, skin, teeth, eye, eyelashes, eyeOcclusion, tearLine, hair, eyebrows };

	// Unreal's hair material: melanin and redness give the base, the dyes are multiplied in by absorption
	struct TestHairColour
	{
		float	melanin = 0.0f;
		float	redness = 0.0f;
		float	dye[ 3 ] { 1.0f, 1.0f, 1.0f };
		float	ombre = 0.0f;					// blend towards the ombre colour along the strand
		float	ombreShift = 0.0f;				// where along the strand the ombre starts
		float	ombreMelanin = 0.0f;
		float	ombreRedness = 0.0f;
		float	ombreDye[ 3 ] { 1.0f, 1.0f, 1.0f };
		float	highlights = 0.0f;				// strands picked by the highlight mask
		float	highlightsIntensity = 0.0f;
		float	highlightsMelanin = 0.0f;
		float	highlightsRedness = 0.0f;
		float	highlightsDye[ 3 ] { 1.0f, 1.0f, 1.0f };
		float	roughness = 0.5f;
		float	specular = 0.5f;
	};

	// A range of the model's indices with its own images (normal maps: green up, like the scan's)
	struct TestModelPart
	{
		uint32_t		firstIndex = 0;
		uint32_t		indexCount = 0;
		TestModelImage	colour;
		TestModelImage	normalMap;
		TestModelImage	specular;
		bool			alphaTest = false;		// colour.a below one half is no surface (eyelashes)
		TestPartKind	kind = TestPartKind::surface;
		TestHairColour	hairColour;				// hair and eyebrows only
	};

	struct TestModel
	{
		std::vector<TestModelVertex>	vertices;
		std::vector<uint32_t>			indices;
		float							center[ 3 ] {};
		float							radius = 1.0f;
		TestModelImage					colour;
		TestModelImage					normalMap;		// tangent space, no tangents in the mesh
		TestModelImage					specular;
		std::vector<TestModelPart>		parts;			// empty: one draw of every index with the images above
		std::vector<std::array<float, 4>>	tangents;	// empty or one per vertex: xyz, w the bitangent's handedness
		std::shared_ptr<const struct TestModelRig>	rig;	// null: the model does not move by itself
	};

	// The model's vertices at a time (blinks, eye saccades, jaw, head turns), the same count and order as
	// model.vertices; false (vertices untouched) when the model has no rig. Deterministic in seconds.
	[[ nodiscard ]] bool poseTestModel ( const TestModel& model, float seconds, std::vector<TestModelVertex>& vertices );

	[[ nodiscard ]] std::optional<TestModel> loadTestHead ( const std::filesystem::path& folder );

	// -model <folder>: a MetaHuman export kept outside the repository, fbx\*_Head.FBX and dcc\<name>\Maps\*.png,
	// grooms\*_CardsMesh_Group*_LOD0.fbx with their atlases and material parameters (Unreal's hair, eyebrows)
	[[ nodiscard ]] std::optional<std::filesystem::path> testModelFolder ();
	[[ nodiscard ]] std::optional<TestModel> loadMetaHumanHead ( const std::filesystem::path& folder );
}
