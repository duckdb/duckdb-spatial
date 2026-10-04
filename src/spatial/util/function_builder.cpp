#include "spatial/util/function_builder.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"

namespace duckdb {

string FunctionBuilder::RemoveIndentAndTrailingWhitespace(const char *ptr) {
	string tmp;
	// Replace all tabs with 4 spaces in ptr
	for (const char *text = ptr; *text; text++) {
		if (*text == '\t') {
			tmp.append("    ");
		} else {
			tmp += *text;
		}
	}

	auto text = tmp.c_str();

	string result;
	// Skip any empty first newlines if present
	while (*text == '\n') {
		text++;
	}

	// Track indent length
	auto indent_start = text;
	while (isspace(*text) && *text != '\n') {
		text++;
	}
	auto indent_len = text - indent_start;
	while (*text) {
		result += *text;
		if (*text++ == '\n') {
			// Remove all indentation, but only if it matches the first line's indentation
			bool matched_indent = true;
			for (auto i = 0; i < indent_len; i++) {
				if (*text != indent_start[i]) {
					matched_indent = false;
					break;
				}
			}
			if (matched_indent) {
				auto remaining_indent = indent_len;
				while (*text && remaining_indent > 0) {
					text++;
					remaining_indent--;
				}
			}
		}
	}

	// Also remove any trailing whitespace
	result.erase(result.find_last_not_of(" \n\r\t") + 1);
	return result;
}

void FunctionBuilder::Register(ExtensionLoader &loader, const char *name, ScalarFunctionBuilder &builder) {
	// Register the function
	loader.RegisterFunction(std::move(builder.set));
	auto &db = loader.GetDatabaseInstance();

	// Also add the parameter names. We need to access the catalog entry for this.
	auto &catalog = Catalog::GetSystemCatalog(db);
	auto transaction = CatalogTransaction::GetSystemTransaction(db);
	auto &schema = catalog.GetSchema(transaction, Identifier::DefaultSchema());
	auto catalog_entry = schema.GetEntry(transaction, CatalogType::SCALAR_FUNCTION_ENTRY, Identifier(name));
	if (!catalog_entry) {
		// This should not happen, we just registered the function
		throw InternalException("Function with name \"%s\" not found in FunctionBuilder::AddScalar", name);
	}

	auto &func_entry = catalog_entry->Cast<FunctionEntry>();

	// Insert all descriptions
	for (auto &desc : builder.descriptions) {

		// Add default description if none is set
		if (desc.description.empty()) {
			desc.description = builder.default_description;
		} else {
			desc.description = RemoveIndentAndTrailingWhitespace(desc.description.c_str());
		}

		// Add default example if none is set
		if (desc.examples.empty()) {
			desc.examples.push_back(builder.default_example);
		} else {
			for (auto &ex : desc.examples) {
				ex = RemoveIndentAndTrailingWhitespace(ex.c_str());
			}
		}

		func_entry.descriptions.push_back(desc);
	}

	if (!builder.tags.empty()) {
		func_entry.tags = std::move(builder.tags);
	}
}

void FunctionBuilder::Register(ExtensionLoader &loader, const char *name, AggregateFunctionBuilder &builder) {
	// Register the function
	loader.RegisterFunction(std::move(builder.set));
	auto &db = loader.GetDatabaseInstance();

	// Also add the parameter names. We need to access the catalog entry for this.
	auto &catalog = Catalog::GetSystemCatalog(db);
	auto transaction = CatalogTransaction::GetSystemTransaction(db);
	auto &schema = catalog.GetSchema(transaction, Identifier::DefaultSchema());
	auto catalog_entry = schema.GetEntry(transaction, CatalogType::AGGREGATE_FUNCTION_ENTRY, Identifier(name));
	if (!catalog_entry) {
		// This should not happen, we just registered the function
		throw InternalException("Function with name \"%s\" not found in FunctionBuilder::AddAggregate", name);
	}

	auto &func_entry = catalog_entry->Cast<FunctionEntry>();

	// Insert all descriptions
	const auto descr = RemoveIndentAndTrailingWhitespace(builder.description.c_str());
	const auto exampl = RemoveIndentAndTrailingWhitespace(builder.example.c_str());
	FunctionDescription function_description;
	function_description.description = descr;
	function_description.examples.push_back(exampl);
	func_entry.descriptions.push_back(function_description);

	if (!builder.tags.empty()) {
		func_entry.tags = std::move(builder.tags);
	}
}

void FunctionBuilder::Register(ExtensionLoader &loader, const char *name, MacroFunctionBuilder &builder) {
	vector<FunctionDescription> descriptions;
	unique_ptr<CreateMacroInfo> combined_info;

	for (auto &def : builder.macros) {
		// Build macro_definition string: "(param1, param2) AS body"
		string macro_def = "(";
		for (idx_t i = 0; i < def.parameters.size(); i++) {
			if (i > 0) {
				macro_def += ", ";
			}
			macro_def += def.parameters[i];
		}
		macro_def += ") AS ";
		macro_def += def.body;

		DefaultMacro macro = {};
		macro.schema = DEFAULT_SCHEMA;
		macro.name = name;
		macro.macro_definition = macro_def.c_str();

		auto info = DefaultFunctionGenerator::CreateInternalMacroInfo(macro);
		if (!combined_info) {
			combined_info = std::move(info);
		} else {
			for (auto &m : info->macros) {
				combined_info->macros.push_back(std::move(m));
			}
		}

		FunctionDescription function_description;
		if (def.description) {
			function_description.description = RemoveIndentAndTrailingWhitespace(def.description);
		}
		if (def.example) {
			function_description.examples.push_back(RemoveIndentAndTrailingWhitespace(def.example));
		}
		descriptions.push_back(function_description);
	}

	if (combined_info) {
		combined_info->descriptions = descriptions;
		loader.RegisterFunction(*combined_info);
	}
}

void FunctionBuilder::AddTableFunctionDocs(ExtensionLoader &loader, const char *name, const char *desc,
                                           const char *example, const InsertionOrderPreservingMap<string> &tags) {

	auto &db = loader.GetDatabaseInstance();
	auto &catalog = Catalog::GetSystemCatalog(db);
	auto transaction = CatalogTransaction::GetSystemTransaction(db);
	auto &schema = catalog.GetSchema(transaction, Identifier::DefaultSchema());
	auto catalog_entry = schema.GetEntry(transaction, CatalogType::TABLE_FUNCTION_ENTRY, Identifier(name));
	if (!catalog_entry) {
		// This should not happen, we just registered the function
		throw InternalException("Function with name \"%s\" not found in FunctionBuilder::AddScalar", name);
	}

	auto &func_entry = catalog_entry->Cast<FunctionEntry>();
	FunctionDescription function_description;
	function_description.description = RemoveIndentAndTrailingWhitespace(desc);
	function_description.examples.push_back(RemoveIndentAndTrailingWhitespace(example));
	func_entry.descriptions.push_back(function_description);

	for (const auto &tag : tags) {
		func_entry.tags.insert(tag.first, tag.second);
	}
}

} // namespace duckdb
