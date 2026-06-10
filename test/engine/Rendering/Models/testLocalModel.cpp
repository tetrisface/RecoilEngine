/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "Rendering/Models/LocalModel.hpp"

#include "Rendering/Models/3DModel.hpp"
#include "Rendering/Models/3DModelPiece.hpp"
#include "Sim/Units/Unit.h"
#include "System/Matrix44f.h"
#include "System/Platform/Threading.h"

#include <catch_amalgamated.hpp>

#include <memory>
#include <vector>

namespace
{
struct TestModel
{
	TestModel(const float3& mins, const float3& maxs)
	{
		auto root = std::make_unique<S3DModelPiece>();

		root->name = "root";
		root->mins = mins;
		root->maxs = maxs;
		root->GetIndicesVec() = {0, 1, 2};
		root->SetParentModel(&model);

		model.name = "test-model";
		model.numPieces = 1;
		model.AddPiece(root.get());

		pieces.emplace_back(std::move(root));
	}

	S3DModel model;
	std::vector<std::unique_ptr<S3DModelPiece>> pieces;
};

void CheckFloat3Equals(const float3& actual, const float3& expected)
{
	CHECK(actual.x == expected.x);
	CHECK(actual.y == expected.y);
	CHECK(actual.z == expected.z);
}
}

namespace Threading
{
bool IsMainThread()
{
	return true;
}

NativeThreadId GetCurrentThreadId()
{
	return 0;
}
}

bool LuaObjectMaterial::SetLODCount(unsigned int count)
{
	lodCount = count;
	lastLOD = (lodCount == 0) ? 0 : lodCount - 1;
	lodMats.resize(count);
	return true;
}

bool LuaObjectMaterial::SetLastLOD(unsigned int lod)
{
	lastLOD = (lodCount == 0) ? 0 : std::min(lod, lodCount - 1);
	return true;
}

LuaMatRef::LuaMatRef(const LuaMatRef& mr)
	: bin(mr.bin)
{}

LuaMatRef& LuaMatRef::operator=(const LuaMatRef& mr)
{
	bin = mr.bin;
	return *this;
}

LuaMatRef::~LuaMatRef() = default;
void LuaMatRef::Reset() { bin = nullptr; }
void LuaMatRef::AddUnit(CSolidObject*) {}
void LuaMatRef::AddFeature(CSolidObject*) {}

float3 S3DModelPiece::GetEmitPos() const
{
	return ZeroVector;
}

float3 S3DModelPiece::GetEmitDir() const
{
	return FwdVector;
}

void S3DModelPiece::PostProcessGeometry(uint32_t) {}

CMatrix44f CUnit::GetTransformMatrix(bool, bool) const
{
	return CMatrix44f();
}

LocalModelPiece::LocalModelPiece(const S3DModelPiece* piece)
	: dirty(true)
	, wasUpdated{true, false}
	, noInterpolation{false, false, false}
	, colvol(nullptr)
	, dir(FwdVector)
	, scriptSetVisible(true)
	, blockScriptAnims(false)
	, lmodelPieceIndex(-1)
	, scriptPieceIndex(-1)
	, original(piece)
	, localModel(nullptr)
{
	REQUIRE(piece != nullptr);

	pos = piece->offset;
	rot = ZeroVector;
	scale = piece->scale;
	pieceSpaceTra = Transform(pos);
	modelSpaceTra = pieceSpaceTra;
	modelSpaceMat = modelSpaceTra.ToMatrix();
	prevModelSpaceTra = modelSpaceTra;
	parent = nullptr;
}

LocalModelPiece::~LocalModelPiece() = default;

void LocalModelPiece::Draw() const {}
void LocalModelPiece::DrawLOD(uint32_t) const {}
void LocalModelPiece::SetLODCount(uint32_t) {}

void LocalModelPiece::UpdateChildTransformRec(bool updateChildTransform) const
{
	if (dirty) {
		dirty = false;
		wasUpdated[0] = true;
		updateChildTransform = true;
		pieceSpaceTra = Transform(pos);
	}

	if (updateChildTransform) {
		modelSpaceTra = (parent != nullptr) ? (parent->modelSpaceTra * pieceSpaceTra) : pieceSpaceTra;
		modelSpaceMat = modelSpaceTra.ToMatrix();
	}

	for (const LocalModelPiece* child: children) {
		child->UpdateChildTransformRec(updateChildTransform);
	}
}

void LocalModelPiece::UpdateParentMatricesRec() const
{
	if (parent != nullptr && parent->dirty)
		parent->UpdateParentMatricesRec();

	dirty = false;
	wasUpdated[0] = true;
	pieceSpaceTra = Transform(pos);
	modelSpaceTra = (parent != nullptr) ? (parent->modelSpaceTra * pieceSpaceTra) : pieceSpaceTra;
	modelSpaceMat = modelSpaceTra.ToMatrix();
}

const Transform& LocalModelPiece::GetModelSpaceTransform() const
{
	if (dirty)
		UpdateParentMatricesRec();

	return modelSpaceTra;
}

const CMatrix44f& LocalModelPiece::GetModelSpaceMatrix() const
{
	if (dirty)
		UpdateParentMatricesRec();

	return modelSpaceMat;
}

void LocalModelPiece::SavePrevModelSpaceTransform()
{
	prevModelSpaceTra = GetModelSpaceTransform();
}

TEST_CASE("LocalModel post-load reattaches pieces without replacing serialized bounds")
{
	TestModel savedModel({-2.0f, -3.0f, -5.0f}, {4.0f, 7.0f, 11.0f});
	TestModel reloadedModel({-20.0f, -30.0f, -50.0f}, {40.0f, 70.0f, 110.0f});

	LocalModel localModel;
	localModel.SetModel(&savedModel.model, true);

	const CollisionVolume* savedVolume = localModel.GetBoundingVolume();
	const float3 savedScales = savedVolume->GetScales();
	const float3 savedOffsets = savedVolume->GetOffsets();
	const float savedRadius = savedVolume->GetBoundingRadius();

	localModel.SetBoundariesNeedsRecalc();

	localModel.SetModel(&reloadedModel.model, false);

	REQUIRE(localModel.GetPiece(0)->original == reloadedModel.model.GetPiece(0));
	CHECK(localModel.GetBoundariesNeedsRecalc());

	const CollisionVolume* restoredVolume = localModel.GetBoundingVolume();
	CheckFloat3Equals(restoredVolume->GetScales(), savedScales);
	CheckFloat3Equals(restoredVolume->GetOffsets(), savedOffsets);
	CHECK(restoredVolume->GetBoundingRadius() == savedRadius);
}
