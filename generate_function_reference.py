import os
import json

# We just take the first non-empty description and example for now
get_spatial_functions_sql = """
INSTALL json;
LOAD json;

SELECT
    json({ 
        name: function_name,
        signatures: signatures,
        tags: func_tags,
        description: description,
        example: example
    })
FROM (
    SELECT
        function_type AS type,
        function_name,
        list({
            return: return_type,
            params: list_zip(parameters, parameter_types)::STRUCT(name VARCHAR, type VARCHAR)[],
            description: description,
            examples: examples
        }) as signatures,
        list_filter(signatures, x -> x.description IS NOT NULL)[1].description as description,
        list_filter(signatures, x -> len(x.examples) != 0)[1].examples[1] as example,
        any_value(tags) AS func_tags,
    FROM duckdb_functions() as funcs
    WHERE function_type = '$FUNCTION_TYPE$'
        -- function-specific tweaks
        AND CASE function_name
            -- TODO: https://github.com/duckdb/duckdb-spatial/pull/601#discussion_r2144753435
            WHEN '&&' THEN 'box' IN parameters
            ELSE true
        END
    GROUP BY function_name, function_type
    HAVING func_tags['ext'] = 'spatial'
        -- TODO: macros cannot have tags
        OR (
            func_tags['ext'] IS NULL
            AND function_name LIKE 'ST_%'
        )
    ORDER BY function_name
);
"""

def get_functions(function_type = 'scalar'):
    functions = []
    for line in os.popen("./build/debug/duckdb -list -noheader -c \"" + get_spatial_functions_sql.replace('$FUNCTION_TYPE$', function_type) + "\"").readlines():
        functions.append(json.loads(line))
    return functions

def write_table_of_contents(f, functions):
    f.write('| Function | Summary |\n')
    f.write('| --- | --- |\n')
    for function in functions:
        # Summary is the first line of the description
        summary = function['description'].split('\n')[0] if function['description'] else ""
        f.write(f"| [`{function['name']}`](#{to_kebab_case(function['name'])}) | {summary} |\n")


def to_kebab_case(name):
    return name.replace(" ", "-").lower()

# TODO: currently, macro functions cannot contain the information avout the
#       parameter types, so do some wild guess.
def guess_param_type_from_name(name):
    if name == "geom":
        return "GEOMETRY"
    return "double"


# TODO: Ditto. Currently, all macro functions return GEOMETRY.
def guess_return_type_from_name(name):
    return "GEOMETRY"


def main():
    with open("./docs/functions.md", "w") as f:

        f.write("# DuckDB Spatial Function Reference\n\n")

        print("Collecting functions")

        aggregate_functions = get_functions('aggregate')
        window_functions = get_functions('window')
        scalar_functions = get_functions('scalar')
        table_functions = get_functions('table')
        macro_functions = get_functions('macro')

        # Write function index
        f.write("## Function Index \n")
        f.write("**[Scalar Functions](#scalar-functions)**\n\n")
        write_table_of_contents(f, scalar_functions)
        f.write("\n")
        f.write("**[Aggregate Functions](#aggregate-functions)**\n\n")
        write_table_of_contents(f, aggregate_functions)
        f.write("\n")
        f.write("**[Window Functions](#window-functions)**\n\n")
        write_table_of_contents(f, window_functions)
        f.write("\n")
        f.write("**[Macro Functions](#Macro-functions)**\n\n")
        write_table_of_contents(f, macro_functions)
        f.write("\n")
        f.write("**[Table Functions](#table-functions)**\n\n")
        write_table_of_contents(f, table_functions)
        f.write("\n")
        f.write("----\n\n")

        # Write basic functions
        for func_set in [
            ("Scalar Functions", scalar_functions),
            ("Aggregate Functions", aggregate_functions),
            ("Window Functions", window_functions),
            ("Macro Functions", macro_functions),
        ]:
            f.write(f"## {func_set[0]}\n\n")
            for function in func_set[1]:
                f.write(f"### {function['name']}\n\n\n")
                #summary = function['description'].split('\n')[0]
                #f.write(f"_{summary}_\n\n")
                
                f.write("#### Signature\n\n") if len(function['signatures']) == 1 else f.write("#### Signatures\n\n")
                f.write("```sql\n")
                for signature in function['signatures']:
                    param_list = ", ".join(
                        [
                            f"{param['name']} {param['type'] if param['type'] else guess_param_type_from_name(param['name'])}"
                            for param in signature["params"]
                        ]
                    )
                    return_type = (
                        signature["return"]
                        if signature["return"]
                        else guess_return_type_from_name(function["name"])
                    )
                    f.write(f"{return_type} {function['name']} ({param_list})\n")
                f.write("```\n\n")



                if function['description']:
                    f.write("#### Description\n\n")
                    f.write(function['description'])
                    f.write("\n\n")
                else:
                    print(f"No description for {function['name']}")

                if function['example']:
                    f.write("#### Example\n\n")
                    f.write("```sql\n")
                    f.write(function['example'])
                    f.write("\n```\n\n")
                else:
                    print(f"No example for {function['name']}")
                #f.write("\n\n")
                f.write("----\n\n")

        # Write table functions
        f.write("## Table Functions\n\n")
        for function in table_functions:
            f.write(f"### {function['name']}\n\n")
            #summary = function['description'].split('\n')[0]
            #f.write(f"_{summary}_\n\n")

            f.write("#### Signature\n\n")
            f.write("```sql\n")
            for signature in function['signatures']:
                param_list = ", ".join([f"{param['name']} {param['type']}" for param in signature['params']])
                f.write(f"{function['name']} ({param_list})\n")
            f.write("```\n\n")

            if function['description']:
                f.write("#### Description\n\n")
                f.write(function['description'])
                f.write("\n\n")
            else:
                print(f"No description for {function['name']}")

            if function['example']:
                f.write("#### Example\n\n")
                f.write("```sql\n")
                f.write(function['example'])
                f.write("\n```\n\n")
            else:
                print(f"No example for {function['name']}")
            f.write("----\n\n")

if __name__ == "__main__":
    main()
