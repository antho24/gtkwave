#include <gtkwave.h>

static void test_var_type_to_string(void)
{
    g_assert_cmpstr(gw_var_type_to_string(GW_VAR_TYPE_GEN_STRING), ==, "string");
    g_assert_cmpstr(gw_var_type_to_string(GW_VAR_TYPE_GEN_MISSING), ==, "missing");
}

static void test_var_data_type_to_string(void)
{
    g_assert_cmpstr(gw_var_data_type_to_string(GW_VAR_DATA_TYPE_VHDL_STRING), ==, "string");
}

static void test_var_dir_to_string(void)
{
    g_assert_cmpstr(gw_var_dir_to_string(GW_VAR_DIR_IMPLICIT), ==, "");
    g_assert_cmpstr(gw_var_dir_to_string(GW_VAR_DIR_IN), ==, "I");
    g_assert_cmpstr(gw_var_dir_to_string(GW_VAR_DIR_LINKAGE), ==, "L");
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/var_enums/var_type_to_string", test_var_type_to_string);
    g_test_add_func("/var_enums/var_data_type_to_string", test_var_data_type_to_string);
    g_test_add_func("/var_enums/var_dir_to_string", test_var_dir_to_string);

    return g_test_run();
}
