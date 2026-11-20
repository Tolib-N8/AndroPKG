// andropkg: production code only needs FRIEND_TEST from gtest.
#pragma once
#define FRIEND_TEST(test_case_name, test_name) friend class test_case_name##_##test_name##_Test
