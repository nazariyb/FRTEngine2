// Transform math is verified against DirectXMath and against the STransform path it
// replaces, rather than against hand-derived expected values - the handedness flip was
// established this way after a derivation got it backwards.
#include <gtest/gtest.h>

#include <DirectXMath.h>

#include "TestCommon.h"

#include "Math/Comp_WorldTransform.h"
#include "Math/Transform.h"

using namespace frt;
using namespace DirectX;

namespace
{
constexpr double Tolerance = 1e-5;
}


// ---------------------------------------------------------------------------------
// TMatrix3x3
// ---------------------------------------------------------------------------------

TEST(Matrix3x3, DefaultsToIdentity)
{
	const Matrix3x3f m;

	for (uint32 row = 0u; row < 3u; ++row)
	{
		for (uint32 col = 0u; col < 3u; ++col)
		{
			EXPECT_NEAR(m.M[row][col], row == col ? 1.0f : 0.0f, Tolerance);
		}
	}
}

/**
 * Ours is column-vector, XMMatrixRotationRollPitchYaw is row-vector, so the same
 * rotation means ours equals theirs transposed.
 */
static void ExpectEulerMatchesDirectX (float InPitch, float InYaw, float InRoll)
{
	SCOPED_TRACE(testing::Message() << "euler(" << InPitch << ", " << InYaw << ", " << InRoll << ")");

	const Matrix3x3f ours = Matrix3x3f::FromEulerRadians(Vector3f(InPitch, InYaw, InRoll));

	XMFLOAT4X4 theirs;
	XMStoreFloat4x4(&theirs, XMMatrixRotationRollPitchYaw(InPitch, InYaw, InRoll));

	for (int row = 0; row < 3; ++row)
	{
		for (int col = 0; col < 3; ++col)
		{
			EXPECT_NEAR(ours.M[row][col], theirs.m[col][row], Tolerance)
				<< "element [" << row << "][" << col << "]";
		}
	}
}

TEST(Matrix3x3, FromEulerMatchesDirectXMath)
{
	ExpectEulerMatchesDirectX(0.0f, 0.0f, 0.0f);
	ExpectEulerMatchesDirectX(0.3f, 0.0f, 0.0f);
	ExpectEulerMatchesDirectX(0.0f, 0.7f, 0.0f);
	ExpectEulerMatchesDirectX(0.0f, 0.0f, 1.1f);
	ExpectEulerMatchesDirectX(0.3f, 0.7f, 1.1f);
	ExpectEulerMatchesDirectX(-1.2f, 2.4f, -0.6f);
	ExpectEulerMatchesDirectX(math::PI_OVER_TWO, 0.0f, 0.0f); // the Ceiling / Portal case
}

TEST(Matrix3x3, InverseUndoesRotationAndNonUniformScale)
{
	const Matrix3x3f m = Matrix3x3f::FromRotationScale(
		Vector3f(0.3f, 0.7f, 1.1f), Vector3f(2.0f, 3.0f, 0.5f));

	const Matrix3x3f roundTrip = m * m.Inverse();

	for (uint32 row = 0u; row < 3u; ++row)
	{
		for (uint32 col = 0u; col < 3u; ++col)
		{
			EXPECT_NEAR(roundTrip.M[row][col], row == col ? 1.0f : 0.0f, Tolerance);
		}
	}
}

TEST(Matrix3x3, TransposeIsInvolutive)
{
	const Matrix3x3f m = Matrix3x3f::FromEulerRadians(Vector3f(0.3f, 0.7f, 1.1f));
	const Matrix3x3f back = m.Transposed().Transposed();

	for (uint32 row = 0u; row < 3u; ++row)
	{
		for (uint32 col = 0u; col < 3u; ++col)
		{
			EXPECT_NEAR(back.M[row][col], m.M[row][col], Tolerance);
		}
	}
}

TEST(Matrix3x3, MixedPrecisionMultiplyPromotesToWider)
{
	// A float basis transforming a double translation must yield double, or the whole
	// float-basis / double-translation split silently truncates.
	const Matrix3x3f basis = Matrix3x3f::FromEulerRadians(Vector3f(0.0f, 0.0f, 0.0f));
	const Vector3d point(1e9 + 0.5, 0.0, 0.0);

	const auto result = basis * point;

	static_assert(std::is_same_v<std::remove_cvref_t<decltype(result)>, Vector3d>,
		"float basis * double vector must produce a double vector");

	EXPECT_NEAR(result.x, 1e9 + 0.5, 1e-6);
}


// ---------------------------------------------------------------------------------
// Comp_WorldTransform
// ---------------------------------------------------------------------------------

/** The full local -> world -> instance path must agree with STransform's output. */
static void ExpectInstanceMatchesSTransform (
	const Vector3f& InTranslation, const Vector3f& InRotation, const Vector3f& InScale)
{
	SCOPED_TRACE(testing::Message()
		<< "T(" << InTranslation.x << "," << InTranslation.y << "," << InTranslation.z << ") "
		<< "S(" << InScale.x << "," << InScale.y << "," << InScale.z << ")");

	Comp_LocalTransformF local;
	local.SetTranslation(InTranslation);
	local.SetRotation(InRotation);
	local.SetScale(InScale);

	const Comp_WorldTransformF world = Comp_WorldTransformF::FromLocal(local);
	const math::TMatrix3x4<float> ours =
		math::ToDirectXHandedness(world.ToInstanceTransform(Vector3f::ZeroVector));

	math::STransform reference;
	reference.SetTranslation(InTranslation);
	reference.SetRotation(InRotation);
	reference.SetScale(InScale);
	const XMFLOAT3X4 theirs = reference.GetRaytracingTransform();

	for (int row = 0; row < 3; ++row)
	{
		for (int col = 0; col < 4; ++col)
		{
			EXPECT_NEAR(ours.M[row][col], theirs.m[row][col], Tolerance)
				<< "element [" << row << "][" << col << "]";
		}
	}
}

TEST(WorldTransform, InstanceTransformMatchesSTransform)
{
	// Uniform scale: the two scale-composition conventions agree, so these must match
	// exactly.
	ExpectInstanceMatchesSTransform(
		Vector3f(1.0f, 2.0f, 3.0f), Vector3f(0.3f, 0.7f, 1.1f), Vector3f(2.0f));
	ExpectInstanceMatchesSTransform(
		Vector3f(-5.0f, 0.5f, 9.0f), Vector3f(-1.2f, 2.4f, -0.6f), Vector3f(1.0f));

	// The real Ceiling entity: non-uniform scale about the same axis it rotates around,
	// so R*S and S*R commute and this matches despite the differing convention.
	ExpectInstanceMatchesSTransform(
		Vector3f(-0.5f, 9.0f, 0.0f), Vector3f(math::PI_OVER_TWO, 0.0f, 0.0f), Vector3f(1.5f, 1.0f, 1.0f));
}

TEST(WorldTransform, ComposeAppliesParentRotationToChildOffset)
{
	Comp_LocalTransformF parentLocal;
	parentLocal.SetTranslation(Vector3f(10.0f, 0.0f, 0.0f));
	parentLocal.SetRotation(Vector3f(0.0f, math::PI_OVER_TWO, 0.0f));

	Comp_LocalTransformF childLocal;
	childLocal.SetTranslation(Vector3f(0.0f, 0.0f, 5.0f));

	const Comp_WorldTransformF parent = Comp_WorldTransformF::FromLocal(parentLocal);
	const Comp_WorldTransformF child = Comp_WorldTransformF::Compose(parent, childLocal);

	// A 90 degree yaw maps +Z onto +X, so the child lands at 10 + 5 on X.
	EXPECT_NEAR(child.Translation.x, 15.0f, Tolerance);
	EXPECT_NEAR(child.Translation.y, 0.0f, Tolerance);
	EXPECT_NEAR(child.Translation.z, 0.0f, Tolerance);
}

TEST(WorldTransform, IdentityComposeIsANoOp)
{
	Comp_LocalTransformF local;
	local.SetTranslation(Vector3f(1.0f, 2.0f, 3.0f));

	const Comp_WorldTransformF identity;
	const Comp_WorldTransformF composed = Comp_WorldTransformF::Compose(identity, local);
	const Comp_WorldTransformF direct = Comp_WorldTransformF::FromLocal(local);

	EXPECT_NEAR(composed.Translation.x, direct.Translation.x, Tolerance);
	EXPECT_NEAR(composed.Translation.y, direct.Translation.y, Tolerance);
	EXPECT_NEAR(composed.Translation.z, direct.Translation.z, Tolerance);
}

TEST(WorldTransform, CameraRelativeSubtractionHappensAtFullPrecision)
{
	// The point of storing absolute translation wide: subtract at double precision, and
	// only the small relative result is narrowed for upload.
	Comp_LocalTransformD local;
	local.SetTranslation(Vector3d(6378137.5, 100.25, -4000000.75));

	const Comp_WorldTransformD world = Comp_WorldTransformD::FromLocal(local);
	const Vector3d camera(6378137.0, 100.0, -4000000.0);

	const math::TMatrix3x4<float> relative = world.ToInstanceTransform(camera);

	EXPECT_NEAR(relative.M[0][3], 0.5f, Tolerance);
	EXPECT_NEAR(relative.M[1][3], 0.25f, Tolerance);
	EXPECT_NEAR(relative.M[2][3], -0.75f, Tolerance);
}

TEST(WorldTransform, HandednessFlipNegatesTheBasisXColumnNotThePosition)
{
	// Right-multiply by diag(-1,1,1): it converts the source basis, so the translation
	// column is deliberately untouched. Negating row 0 instead would also mirror the
	// world position, which is NOT what STransform does.
	math::TMatrix3x4<float> m;
	m.M[0][0] = 1.0f; m.M[1][0] = 2.0f; m.M[2][0] = 3.0f;
	m.M[0][3] = 7.0f; m.M[1][3] = 8.0f; m.M[2][3] = 9.0f;

	const math::TMatrix3x4<float> flipped = math::ToDirectXHandedness(m);

	EXPECT_NEAR(flipped.M[0][0], -1.0f, Tolerance);
	EXPECT_NEAR(flipped.M[1][0], -2.0f, Tolerance);
	EXPECT_NEAR(flipped.M[2][0], -3.0f, Tolerance);

	EXPECT_NEAR(flipped.M[0][3], 7.0f, Tolerance) << "translation must not be mirrored";
	EXPECT_NEAR(flipped.M[1][3], 8.0f, Tolerance);
	EXPECT_NEAR(flipped.M[2][3], 9.0f, Tolerance);
}

TEST(WorldTransform, TransformPointAppliesBasisThenTranslation)
{
	Comp_LocalTransformF local;
	local.SetTranslation(Vector3f(10.0f, 0.0f, 0.0f));
	local.SetRotation(Vector3f(0.0f, math::PI_OVER_TWO, 0.0f));

	const Comp_WorldTransformF world = Comp_WorldTransformF::FromLocal(local);
	const Vector3f transformed = world.TransformPoint(Vector3f(0.0f, 0.0f, 1.0f));

	EXPECT_NEAR(transformed.x, 11.0f, Tolerance);
	EXPECT_NEAR(transformed.y, 0.0f, Tolerance);
	EXPECT_NEAR(transformed.z, 0.0f, Tolerance);
}


// ---------------------------------------------------------------------------------
// Comp_LocalTransform precision behaviour
// ---------------------------------------------------------------------------------

TEST(LocalTransform, DefaultsToIdentity)
{
	const Comp_LocalTransformF local;

	EXPECT_EQ(local.Translation.x, 0.0f);
	EXPECT_EQ(local.Rotation.x, 0.0f);
	EXPECT_EQ(local.Scale.x, 1.0f) << "scale must default to one, not zero";
	EXPECT_EQ(local.Scale.y, 1.0f);
	EXPECT_EQ(local.Scale.z, 1.0f);
}

TEST(LocalTransform, NarrowingSettersStillConvertCorrectly)
{
	// The narrowing overloads warn at the call site (C4996 via FRT_WARN_ON_USE), which is
	// a compile-time behaviour the test binary cannot observe. What IS testable is that
	// they convert correctly when deliberately used.
#pragma warning(push)
#pragma warning(disable: 4996)
	Comp_LocalTransformF local;
	local.SetTranslation(1.5, 2.5, 3.5);
	local.SetScale(Vector3d(2.0, 2.0, 2.0));
#pragma warning(pop)

	EXPECT_NEAR(local.Translation.x, 1.5f, Tolerance);
	EXPECT_NEAR(local.Translation.z, 3.5f, Tolerance);
	EXPECT_NEAR(local.Scale.y, 2.0f, Tolerance);
}

TEST(LocalTransform, ExplicitPrecisionConversionRoundTrips)
{
	Comp_LocalTransformD wide;
	wide.SetTranslation(Vector3d(1.25, 2.5, 3.75));
	wide.SetScale(Vector3d(2.0, 3.0, 4.0));

	const Comp_LocalTransformF narrow = wide.To<float>();

	EXPECT_NEAR(narrow.Translation.x, 1.25f, Tolerance);
	EXPECT_NEAR(narrow.Translation.z, 3.75f, Tolerance);
	EXPECT_NEAR(narrow.Scale.y, 3.0f, Tolerance);
}
