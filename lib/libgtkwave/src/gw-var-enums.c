#include "gw-var-enums.h"
#include <string.h>

#define WAVE_NODEVARTYPE_STR

const gchar *gw_var_type_to_string(GwVarType self)
{
    g_return_val_if_fail(self >= 0 && self <= GW_VAR_TYPE_MAX, "");

    static const char *STRINGS[] = {
        "",        "event", "integer", "parm",   "real",     "real_parm", "reg",    "supply0",
        "supply1", "time",  "tri",     "triand", "trior",    "trireg",    "tri0",   "tri1",
        "wand",    "wire",  "wor",     "array",  "realtime", "port",      "string", "bit",
        "logic",   "int",   "s_int",   "l_int",  "byte",     "enum",      "s_real", "signal",
        "var",     "const", "file",    "memory", "net",      "alias",     "missing"};

    return STRINGS[self];
}

const gchar *gw_var_data_type_to_string(GwVarDataType self)
{
    g_return_val_if_fail(self >= 0 && self <= GW_VAR_DATA_TYPE_MAX, "");

    static const gchar *STRINGS[] = {"",
                                     "boolean",
                                     "bit",
                                     "bit_vec",
                                     "ulogic",
                                     "ulogic_v",
                                     "logic",
                                     "logic_v",
                                     "unsigned",
                                     "signed",
                                     "integer",
                                     "real",
                                     "natural",
                                     "positive",
                                     "time",
                                     "char",
                                     "string",
                                     "ufixed",
                                     "sfixed"};

    return STRINGS[self];
}

/**
 * gw_var_data_type_from_vhdl_type_name:
 * @name: The name of a VHDL type, e.g. "unsigned" or "IEEE.NUMERIC_STD.UNSIGNED".
 *
 * Maps the names of the standard VHDL types to their data type. The resolved and unresolved
 * variants of a type (e.g. "unresolved_unsigned" or "u_unsigned") map to the same data type.
 *
 * Returns: The data type or %GW_VAR_DATA_TYPE_NONE for other types.
 */
GwVarDataType gw_var_data_type_from_vhdl_type_name(const gchar *name)
{
    static const struct
    {
        const gchar *name;
        GwVarDataType data_type;
    } TYPES[] = {
        {"boolean", GW_VAR_DATA_TYPE_VHDL_BOOLEAN},
        {"bit", GW_VAR_DATA_TYPE_VHDL_BIT},
        {"bit_vector", GW_VAR_DATA_TYPE_VHDL_BIT_VECTOR},
        {"std_ulogic", GW_VAR_DATA_TYPE_VHDL_STD_ULOGIC},
        {"std_ulogic_vector", GW_VAR_DATA_TYPE_VHDL_STD_ULOGIC_VECTOR},
        {"std_logic", GW_VAR_DATA_TYPE_VHDL_STD_LOGIC},
        {"std_logic_vector", GW_VAR_DATA_TYPE_VHDL_STD_LOGIC_VECTOR},
        {"unsigned", GW_VAR_DATA_TYPE_VHDL_UNSIGNED},
        {"signed", GW_VAR_DATA_TYPE_VHDL_SIGNED},
        {"integer", GW_VAR_DATA_TYPE_VHDL_INTEGER},
        {"real", GW_VAR_DATA_TYPE_VHDL_REAL},
        {"natural", GW_VAR_DATA_TYPE_VHDL_NATURAL},
        {"positive", GW_VAR_DATA_TYPE_VHDL_POSITIVE},
        {"time", GW_VAR_DATA_TYPE_VHDL_TIME},
        {"character", GW_VAR_DATA_TYPE_VHDL_CHARACTER},
        {"string", GW_VAR_DATA_TYPE_VHDL_STRING},
        {"ufixed", GW_VAR_DATA_TYPE_VHDL_UFIXED},
        {"sfixed", GW_VAR_DATA_TYPE_VHDL_SFIXED},
    };

    g_return_val_if_fail(name != NULL, GW_VAR_DATA_TYPE_NONE);

    // strip the library and package names
    const gchar *dot = strrchr(name, '.');
    if (dot != NULL) {
        name = dot + 1;
    }

    if (g_ascii_strncasecmp(name, "unresolved_", strlen("unresolved_")) == 0) {
        name += strlen("unresolved_");
    } else if (g_ascii_strncasecmp(name, "u_", strlen("u_")) == 0) {
        name += strlen("u_");
    }

    for (gsize i = 0; i < G_N_ELEMENTS(TYPES); i++) {
        if (g_ascii_strcasecmp(name, TYPES[i].name) == 0) {
            return TYPES[i].data_type;
        }
    }

    return GW_VAR_DATA_TYPE_NONE;
}

const gchar *gw_var_dir_to_string(GwVarDir self)
{
    g_return_val_if_fail(self >= 0 && self <= GW_VAR_DIR_MAX, "");

    static const gchar *STRINGS[] = {"", "I", "O", "IO", "B", "L"};

    return STRINGS[self];
}
