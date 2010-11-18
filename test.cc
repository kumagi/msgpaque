
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <assert.h>

#define NEVER_REACH EXPECT_TRUE(false)

using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::StrictMock;
using ::testing::AtLeast;
using ::testing::_;

