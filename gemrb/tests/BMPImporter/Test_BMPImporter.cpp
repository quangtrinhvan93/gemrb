/* GemRB - Infinity Engine Emulator
 * Copyright (C) 2024 The GemRB Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "../../core/Streams/FileStream.h"
#include "../../plugins/BMPImporter/BMPImporter.h"

#include <gtest/gtest.h>

namespace GemRB {

static const path_t resources = PathJoin("tests", "resources", "BMPImporter");
static const path_t SAMPLE_FILE = PathJoin(resources, "sample.bmp");
static const path_t SAMPLE_FILE_8B = PathJoin(resources, "sample_8bit.bmp");
static const path_t SAMPLE_FILE_V3 = PathJoin(resources, "sample_v3.bmp");
static const path_t SAMPLE_FILE_V5 = PathJoin(resources, "sample_v5.bmp");

class BMPImporterTest : public testing::TestWithParam<path_t> {
protected:
	BMPImporter unit;
	const path_t path;

public:
	void SetUp() override
	{
		auto stream = new FileStream {};

		assert(stream->Open(GetParam()));
		assert(unit.Open(stream));
	}
};

// More like a smoke test
TEST_P(BMPImporterTest, GetPalette)
{
	Palette pal;
	EXPECT_EQ(unit.GetPalette(2, pal), -1);
}

INSTANTIATE_TEST_SUITE_P(
	BMPImporterInstances,
	BMPImporterTest,
	testing::Values(
		SAMPLE_FILE,
		SAMPLE_FILE_8B,
		SAMPLE_FILE_V3,
		SAMPLE_FILE_V5));

}
