#include <gtkwave.h>

static void test_var_type_to_string(void)
{
    g_assert_cmpstr(gw_var_type_to_string(GW_VAR_TYPE_GEN_STRING), ==, "string");
    g_assert_cmpstr(gw_var_type_to_string(GW_VAR_TYPE_GEN_MISSING), ==, "missing");
}

static void test_var_data_type_to_string(void)
{
    g_assert_cmpstr(gw_var_data_type_to_string(GW_VAR_DATA_TYPE_VHDL_STRING), ==, "string");
    g_assert_cmpstr(gw_var_data_type_to_string(GW_VAR_DATA_TYPE_VHDL_SFIXED), ==, "sfixed");
    g_assert_cmpstr(gw_var_data_type_to_string(GW_VAR_DATA_TYPE_MAX), ==, "sfixed");
}

static void test_var_data_type_from_vhdl_type_name(void)
{
    g_assert_cmpint(gw_var_data_type_from_vhdl_type_name("unsigned"),
                    ==,
                    GW_VAR_DATA_TYPE_VHDL_UNSIGNED);
    g_assert_cmpint(gw_var_data_type_from_vhdl_type_name("SIGNED"),
                    ==,
                    GW_VAR_DATA_TYPE_VHDL_SIGNED);
    g_assert_cmpint(gw_var_data_type_from_vhdl_type_name("ieee.fixed_pkg.UNRESOLVED_SFIXED"),
                    ==,
                    GW_VAR_DATA_TYPE_VHDL_SFIXED);
    g_assert_cmpint(gw_var_data_type_from_vhdl_type_name("u_ufixed"),
                    ==,
                    GW_VAR_DATA_TYPE_VHDL_UFIXED);
    g_assert_cmpint(gw_var_data_type_from_vhdl_type_name("Natural"),
                    ==,
                    GW_VAR_DATA_TYPE_VHDL_NATURAL);
    g_assert_cmpint(gw_var_data_type_from_vhdl_type_name("state_t"), ==, GW_VAR_DATA_TYPE_NONE);
    g_assert_cmpint(gw_var_data_type_from_vhdl_type_name("unsigned_byte"),
                    ==,
                    GW_VAR_DATA_TYPE_NONE);
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
    g_test_add_func("/var_enums/var_data_type_from_vhdl_type_name",
                    test_var_data_type_from_vhdl_type_name);
    g_test_add_func("/var_enums/var_dir_to_string", test_var_dir_to_string);

    return g_test_run();
}
