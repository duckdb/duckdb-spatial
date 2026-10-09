// Spatial
#include "spatial/modules/gdal/gdal_module.hpp"
#include "spatial/util/function_builder.hpp"

// DUCKDB
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <utility>

// GDAL
#include "gdal.h"
#include "ogr_core.h"
#include "ogr_api.h"
#include "ogr_srs_api.h"
#include "ogrsf_frmts.h"
#include "cpl_string.h"
#include "cpl_error.h"
#include "cpl_vsi.h"
#include "cpl_vsi_error.h"
#include "cpl_vsi_virtual.h"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "spatial/spatial_settings.hpp"
#include "spatial/modules/proj/proj_module.hpp"

namespace duckdb {
namespace {

//======================================================================================================================
// GDAL Error Handling
//======================================================================================================================
// GDAL's internals are not exception-safe: throwing a C++ exception from a CPLErrorHandler (or a VSI callback) unwinds
// through GDAL's own C/C++ frames and leaks heap allocations that would otherwise be freed on the normal return path.
// Instead we install a non-throwing handler that just silences GDAL's default stderr output, and inspect GDAL's
// thread-local error state at our own call boundaries.

// Map GDAL's thread-local last-error state to a DuckDB exception and throw it. Call this only once a GDAL call has
// already reported failure through its return value. `fallback` is used when GDAL recorded no fresh failure of its own.
// We reset GDAL's error state before throwing so a recovered/stale error can't leak onto the next call on this thread.

// Parse the DuckDB message part out of a raw GDAL message (GDAL re-reports caught exceptions as CPLE_AppDefined) and
// strip any /vsiduckdb-<uuid>/ prefix to make the error more readable when we print file paths.
string CleanGDALMessage(const char *raw_msg) {
	ErrorData error_data(raw_msg);
	auto msg = error_data.RawMessage();
	const auto path_pos = msg.find("/vsiduckdb-");
	if (path_pos != string::npos) {
		msg.erase(path_pos, 48);
	}
	return msg;
}

[[noreturn]] void ThrowGDALError(const string &fallback) {

	// If GDAL didn't report a fresh error, but we still want to throw, use the fallback message on its own.
	if (CPLGetLastErrorType() < CE_Failure) {
		CPLErrorReset();
		throw IOException(fallback);
	}

	const auto code = CPLGetLastErrorNo();

	auto msg = CleanGDALMessage(CPLGetLastErrorMsg());
	if (msg.empty()) {
		msg = fallback;
	}

	// Reset GDAL's error state before throwing so a recovered/stale error can't leak onto the next call on this thread.
	CPLErrorReset();

	switch (code) {
	case CPLE_NoWriteAccess:
		throw PermissionException(msg);
	case CPLE_UserInterrupt:
		throw InterruptException();
	case CPLE_OutOfMemory:
		throw OutOfMemoryException(msg);
	case CPLE_NotSupported:
		throw NotImplementedException(msg);
	case CPLE_AssertionFailed:
	case CPLE_ObjectNull:
		throw InternalException(msg);
	case CPLE_IllegalArg:
		throw InvalidInputException(msg);
	case CPLE_AppDefined:
	case CPLE_HttpResponse:
	case CPLE_FileIO:
	case CPLE_OpenFailed:
	default:
		throw IOException(msg);
	}
}

// Run a DuckDB FileSystem operation from within a GDAL VSI callback, converting any C++ exception into a VSI error plus
// the given error return value. Exceptions must never cross the C VSI boundary
template <class FUNC>
auto VSIGuard(FUNC &&func, decltype(std::declval<FUNC &>()()) error_value) -> decltype(std::declval<FUNC &>()()) {
	try {
		return func();
	} catch (std::exception &ex) {
		VSIError(VSIE_FileError, "%s", CleanGDALMessage(ex.what()).c_str());
		return error_value;
	} catch (...) {
		VSIError(VSIE_FileError, "Unknown error in GDAL VSI callback");
		return error_value;
	}
}

//======================================================================================================================
// GDAL FILE
//======================================================================================================================
class DuckDBFileHandle final : public VSIVirtualHandle {
public:
	explicit DuckDBFileHandle(unique_ptr<FileHandle> file_handle_p)
	    : file_handle(std::move(file_handle_p)), is_eof(false), can_seek(file_handle->CanSeek()) {
	}

	vsi_l_offset Tell() override {
		return VSIGuard([&]() -> vsi_l_offset { return static_cast<vsi_l_offset>(file_handle->SeekPosition()); },
		                static_cast<vsi_l_offset>(-1));
	}

	int Seek(vsi_l_offset nOffset, int nWhence) override {
		return VSIGuard(
		    [&]() -> int {
			    // Reset EOF flag on seek
			    is_eof = false;

			    // Use the reset function instead to allow compressed file handles to rewind
			    // even if they don't support seeking
			    if (nWhence == SEEK_SET && nOffset == 0) {
				    file_handle->Reset();
				    return 0;
			    }

			    switch (nWhence) {
			    case SEEK_SET:
				    file_handle->Seek(nOffset);
				    return 0;
			    case SEEK_CUR:
				    file_handle->Seek(file_handle->SeekPosition() + nOffset);
				    return 0;
			    case SEEK_END:
				    file_handle->Seek(file_handle->GetFileSize() + nOffset);
				    return 0;
			    default:
				    return -1;
			    }
		    },
		    -1);
	}

	size_t Read(void *buffer, size_t size, size_t count) override {
		auto bytes_data = static_cast<char *>(buffer);
		auto bytes_left = size * count;

		try {
			while (bytes_left > 0) {
				const auto bytes_read = file_handle->Read(bytes_data, bytes_left);
				if (bytes_read == 0) {
					is_eof = true;
					break;
				}
				bytes_left -= bytes_read;
				bytes_data += bytes_read;
			}
		} catch (std::exception &ex) {
			// Never let the exception cross into GDAL (it is not exception-safe). A clean EOF is reported as EOF;
			// otherwise record a VSI error and return the partial count. Note: GDAL 3.8.5 can't distinguish a short
			// read from an error (fixed in 3.9.2), so a real error here may be observed as EOF by the caller.
			if (file_handle->SeekPosition() == file_handle->GetFileSize()) {
				is_eof = true;
			} else {
				VSIError(VSIE_FileError, "%s", CleanGDALMessage(ex.what()).c_str());
			}
		} catch (...) {
			VSIError(VSIE_FileError, "Unknown error reading file");
		}

		return count - (bytes_left / size);
	}

	int Eof() override {
		return is_eof ? TRUE : FALSE;
	}

	size_t Write(const void *buffer, size_t size, size_t count) override {
		size_t written_bytes = 0;
		try {
			written_bytes = file_handle->Write(const_cast<void *>(buffer), size * count);
		} catch (std::exception &ex) {
			VSIError(VSIE_FileError, "%s", CleanGDALMessage(ex.what()).c_str());
		} catch (...) {
			VSIError(VSIE_FileError, "Unknown error writing file");
		}
		return written_bytes / size;
	}

	int Flush() override {
		return VSIGuard(
		    [&]() -> int {
			    file_handle->Sync();
			    return 0;
		    },
		    -1);
	}
	int Truncate(vsi_l_offset nNewSize) override {
		return VSIGuard(
		    [&]() -> int {
			    file_handle->Truncate(static_cast<int64_t>(nNewSize));
			    return 0;
		    },
		    -1);
	}
	int Close() override {
		return VSIGuard(
		    [&]() -> int {
			    file_handle->Close();
			    return 0;
		    },
		    -1);
	}

private:
	unique_ptr<FileHandle> file_handle = nullptr;
	bool is_eof = false;
	bool can_seek = false;
};

class DuckDBFileSystemHandler final : public VSIFilesystemHandler {
public:
	DuckDBFileSystemHandler(string client_prefix, ClientContext &context)
	    : client_prefix(std::move(client_prefix)), context(context) {};

	const char *StripPrefix(const char *pszFilename) const {
		return pszFilename + client_prefix.size();
	}
	string AddPrefix(const string &value) const {
		return client_prefix + value;
	}

	VSIVirtualHandle *Open(const char *gdal_file_path, const char *access, bool set_error,
	                       CSLConstList /*papszoptions */) override {

		// Strip the prefix to get the real file path
		const auto real_file_path = StripPrefix(gdal_file_path);

		// Get the DuckDB file system
		auto &fs = FileSystem::GetFileSystem(context);

		// Determine the file open flags
		FileOpenFlags flags;
		const auto len = strlen(access);
		if (access[0] == 'r') {
			flags = FileFlags::FILE_FLAGS_READ;
			if (len > 1 && access[1] == '+') {
				flags |= FileFlags::FILE_FLAGS_WRITE;
			}
			if (len > 2 && access[2] == '+') {
				// might be "rb+"
				flags |= FileFlags::FILE_FLAGS_WRITE;
			}
		} else if (access[0] == 'w') {
			flags = FileFlags::FILE_FLAGS_WRITE;
			if (!fs.IsPipe(real_file_path)) {
				flags |= FileFlags::FILE_FLAGS_FILE_CREATE_NEW;
			}
			if (len > 1 && access[1] == '+') {
				flags |= FileFlags::FILE_FLAGS_READ;
			}
			if (len > 2 && access[2] == '+') {
				// might be "wb+"
				flags |= FileFlags::FILE_FLAGS_READ;
			}
		} else if (access[0] == 'a') {
			flags = FileFlags::FILE_FLAGS_APPEND;
			if (len > 1 && access[1] == '+') {
				flags |= FileFlags::FILE_FLAGS_READ;
			}
			if (len > 2 && access[2] == '+') {
				// might be "ab+"
				flags |= FileFlags::FILE_FLAGS_READ;
			}
		} else {
			VSIError(VSIE_FileError, "Unknown file access type: %s", access);
			return nullptr;
		}

		try {
			auto file = fs.OpenFile(real_file_path, flags | FileCompressionType::AUTO_DETECT);
			return new DuckDBFileHandle(std::move(file));

		} catch (std::exception &ex) {

			// Extract error message from DuckDB
			const ErrorData error_data(ex);

			// Failed to open file via DuckDB File System. If this doesnt have a VSI prefix we can return an error here.
			if (strncmp(real_file_path, "/vsi", 4) != 0) {
				if (set_error) {
					VSIError(VSIE_FileError, "%s", error_data.RawMessage().c_str());
				}
				return nullptr;
			}

			// Fall back to GDAL instead (if external access is enabled)
			if (!Settings::Get<EnableExternalAccessSetting>(context)) {
				if (set_error) {
					VSIError(VSIE_FileError, "%s", error_data.RawMessage().c_str());
				}
				return nullptr;
			}

			const auto handler = VSIFileManager::GetHandler(real_file_path);
			if (!handler) {
				if (set_error) {
					VSIError(VSIE_FileError, "%s", error_data.RawMessage().c_str());
				}
				return nullptr;
			}

			return handler->Open(real_file_path, access);
		}
	}

	int Stat(const char *gdal_file_name, VSIStatBufL *result, int n_flags) override {
		return VSIGuard(
		    [&]() -> int {
			    auto real_file_path = StripPrefix(gdal_file_name);
			    auto &fs = FileSystem::GetFileSystem(context);

			    memset(result, 0, sizeof(VSIStatBufL));

			    if (fs.IsPipe(real_file_path)) {
				    result->st_mode = S_IFCHR;
				    return 0;
			    }

			    if (!(fs.FileExists(real_file_path) ||
			          (!FileSystem::IsRemoteFile(real_file_path) && fs.DirectoryExists(real_file_path)))) {
				    return -1;
			    }

#ifdef _WIN32
			    if (!FileSystem::IsRemoteFile(real_file_path) && fs.DirectoryExists(real_file_path)) {
				    result->st_mode = S_IFDIR;
				    return 0;
			    }
#endif

			    FileOpenFlags flags;
			    flags |= FileFlags::FILE_FLAGS_READ;
			    flags |= FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS;
			    flags |= FileCompressionType::AUTO_DETECT;

			    const auto file = fs.OpenFile(real_file_path, flags);
			    if (!file) {
				    return -1;
			    }

			    try {
				    result->st_size = static_cast<off_t>(fs.GetFileSize(*file));
			    } catch (...) {
			    }
			    try {
				    result->st_mtime = Timestamp::ToTimeT(fs.GetLastModifiedTime(*file));
			    } catch (...) {
			    }
			    try {
				    const auto type = file->GetType();
				    switch (type) {
				    case FileType::FILE_TYPE_REGULAR:
					    result->st_mode = S_IFREG;
					    break;
				    case FileType::FILE_TYPE_DIR:
					    result->st_mode = S_IFDIR;
					    break;
				    case FileType::FILE_TYPE_CHARDEV:
					    result->st_mode = S_IFCHR;
					    break;
				    default:
					    // HTTPFS returns invalid type for everything basically.
					    if (FileSystem::IsRemoteFile(real_file_path)) {
						    result->st_mode = S_IFREG;
					    } else {
						    return -1;
					    }
				    }
			    } catch (...) {
			    }
			    return 0;
		    },
		    -1);
	}

	bool IsLocal(const char *gdal_file_path) override {
		const auto real_file_path = StripPrefix(gdal_file_path);
		return !FileSystem::IsRemoteFile(real_file_path);
	}

	int Mkdir(const char *pszDirname, long nMode) override {
		return VSIGuard(
		    [&]() -> int {
			    auto &fs = FileSystem::GetFileSystem(context);
			    const auto dir_name = StripPrefix(pszDirname);

			    fs.CreateDirectory(dir_name);
			    return 0;
		    },
		    -1);
	}

	int Rmdir(const char *pszDirname) override {
		return VSIGuard(
		    [&]() -> int {
			    auto &fs = FileSystem::GetFileSystem(context);
			    const auto dir_name = StripPrefix(pszDirname);

			    fs.RemoveDirectory(dir_name);
			    return 0;
		    },
		    -1);
	}

	int RmdirRecursive(const char *pszDirname) override {
		return VSIGuard(
		    [&]() -> int {
			    auto &fs = FileSystem::GetFileSystem(context);
			    const auto dir_name = StripPrefix(pszDirname);

			    fs.RemoveDirectory(dir_name);
			    return 0;
		    },
		    -1);
	}

	char **ReadDirEx(const char *gdal_dir_name, int max_files) override {
		return VSIGuard(
		    [&]() -> char ** {
			    auto &fs = FileSystem::GetFileSystem(context);
			    const auto dir_name = StripPrefix(gdal_dir_name);

			    CPLStringList files;
			    auto files_count = 0;
			    fs.ListFiles(dir_name, [&](const string &file_name, bool is_dir) {
				    if (files_count >= max_files) {
					    return;
				    }
				    const auto tmp = AddPrefix(file_name);
				    files.AddString(tmp.c_str());
				    files_count++;
			    });
			    return files.StealList();
		    },
		    nullptr);
	}

	char **SiblingFiles(const char *gdal_file_path) override {
		return VSIGuard(
		    [&]() -> char ** {
			    auto &fs = FileSystem::GetFileSystem(context);

			    const auto real_file_path = StripPrefix(gdal_file_path);

			    const auto real_file_stem = StringUtil::GetFileStem(real_file_path);
			    const auto base_file_path = fs.JoinPath(StringUtil::GetFilePath(real_file_path), real_file_stem);
			    const auto glob_file_path = base_file_path + ".*";

			    if (fs.IsRemoteFile(base_file_path)) {
				    // Sibling file listing is expensive for remote files, so avoid it here.
				    // GDAL will fall back to a ReadDir if needed.
				    return nullptr;
			    }

			    CPLStringList files;
			    for (auto &file : fs.Glob(glob_file_path)) {
				    files.AddString(AddPrefix(file.path).c_str());
			    }
			    return files.StealList();
		    },
		    nullptr);
	}

	int HasOptimizedReadMultiRange(const char *pszPath) override {
		return 0;
	}

	int Unlink(const char *prefixed_file_name) override {
		auto &fs = FileSystem::GetFileSystem(context);
		const auto real_file_path = StripPrefix(prefixed_file_name);
		try {
			fs.RemoveFile(real_file_path);
			return 0;
		} catch (...) {
			return -1;
		}
	}

	int Rename(const char *oldpath, const char *newpath) override {
		auto &fs = FileSystem::GetFileSystem(context);
		const auto real_old_path = StripPrefix(oldpath);
		const auto real_new_path = StripPrefix(newpath);

		try {
			fs.MoveFile(real_old_path, real_new_path);
			return 0;
		} catch (...) {
			return -1;
		}
	}

private:
	string client_prefix;
	ClientContext &context;
};

class DuckDBFileSystemPrefix final : public ClientContextState {
public:
	// Use an intentionally leaked heap-allocated mutex to avoid static destruction order issues.
	// A static mutex (either class-level or function-local) can be destroyed before the last
	// DuckDBFileSystemPrefix destructor runs during program teardown, causing
	// "mutex lock failed: Invalid argument". By heap-allocating and never freeing, we guarantee
	// the mutex outlives all users.
	static mutex &GetVSIMutex() {
		static mutex *mtx = new mutex();
		return *mtx;
	}

	explicit DuckDBFileSystemPrefix(ClientContext &context) : context(context) {
		// Create a new random prefix for this client
		client_prefix = StringUtil::Format("/vsiduckdb-%s/", UUID::ToString(UUID::GenerateRandomUUID()));

		// Create a new file handler responding to this prefix
		fs_handler = make_uniq<DuckDBFileSystemHandler>(client_prefix, context);

		// Register the file handler
		lock_guard<mutex> lock(GetVSIMutex());
		VSIFileManager::InstallHandler(client_prefix, fs_handler.get());
	}

	// Delete copy
	DuckDBFileSystemPrefix(const DuckDBFileSystemPrefix &) = delete;
	DuckDBFileSystemPrefix &operator=(const DuckDBFileSystemPrefix &) = delete;

	~DuckDBFileSystemPrefix() override {
		// Uninstall the file handler for this prefix
		lock_guard<mutex> lock(GetVSIMutex());
		VSIFileManager::RemoveHandler(client_prefix);
	}

	// Check if a path looks like a GDAL driver-prefixed URL (e.g., "WFS:https://...", "OAPIF:https://...")
	// These need to be passed directly to GDALOpenEx without our custom VSI prefix.
	static bool IsGDALDriverPrefixedURL(const string &value) {
		auto colon_pos = value.find(':');
		if (colon_pos == string::npos || colon_pos == 0 || colon_pos > 20) {
			return false;
		}
		// Check that the prefix is all uppercase letters (GDAL driver names are uppercase)
		for (idx_t i = 0; i < colon_pos; i++) {
			char c = value[i];
			if (!StringUtil::CharacterIsAlpha(c) || c != StringUtil::CharacterToUpper(c)) {
				return false;
			}
		}
		// Check that after the colon there is a URL scheme (http:// or https://)
		// This excludes database connection strings like "PG:dbname=..." which are not supported.
		auto rest = value.substr(colon_pos + 1);
		return StringUtil::StartsWith(rest, "http://") || StringUtil::StartsWith(rest, "https://");
	}

	string AddPrefix(const string &value) const {
		// If the user explicitly asked for a VSI prefix, we don't add our own
		if (StringUtil::StartsWith(value, "/vsi")) {
			if (!Settings::Get<EnableExternalAccessSetting>(context)) {
				throw PermissionException("Cannot open file '%s' with VSI prefix: External access is disabled", value);
			}
			return value;
		}
		// If the path is a GDAL driver-prefixed URL (e.g., "WFS:https://..."), pass it through directly
		if (IsGDALDriverPrefixedURL(value)) {
			if (!Settings::Get<EnableExternalAccessSetting>(context)) {
				throw PermissionException("Cannot open file '%s' with GDAL driver prefix: External access is disabled",
				                          value);
			}
			return value;
		}
		return client_prefix + value;
	}

	static DuckDBFileSystemPrefix &GetOrCreate(ClientContext &context) {
		return *context.registered_state->GetOrCreate<DuckDBFileSystemPrefix>("gdal", context);
	}

private:
	ClientContext &context;
	string client_prefix;
	unique_ptr<DuckDBFileSystemHandler> fs_handler;
};

//======================================================================================================================
// GDAL READ
//======================================================================================================================
namespace gdal_read {

//----------------------------------------------------------------------------------------------------------------------
// BIND
//----------------------------------------------------------------------------------------------------------------------
class BindData final : public TableFunctionData {
public:
	string real_file_path;
	string gdal_file_path;

	int layer_idx = 0;
	bool keep_wkb = false;

	CPLStringList layer_options;
	CPLStringList dataset_options;
	CPLStringList dataset_sibling;
	CPLStringList dataset_drivers;

	int64_t estimated_cardinality = -1;
	unordered_set<idx_t> geometry_columns = {};

	bool can_filter = false;
	bool has_extent = false;
	bool has_filter = false;
	OGREnvelope layer_extent;
	OGREnvelope layer_filter;

	OGRwkbGeometryType layer_type = wkbUnknown;
};

auto Bind(ClientContext &ctx, TableFunctionBindInput &input, vector<LogicalType> &col_types, vector<Identifier> &col_names)
    -> unique_ptr<FunctionData> {

	auto result = make_uniq<BindData>();

	// Get the file prefix associated with this connection
	const auto &file_prefix = DuckDBFileSystemPrefix::GetOrCreate(ctx);

	// Pass file path
	result->real_file_path = input.inputs[0].GetValue<string>();
	result->gdal_file_path = file_prefix.AddPrefix(result->real_file_path);

	// Parse options
	const auto dataset_options_param = input.named_parameters.find("open_options");
	if (dataset_options_param != input.named_parameters.end()) {
		for (auto &param : ListValue::GetChildren(dataset_options_param->second)) {
			result->dataset_options.AddString(StringValue::Get(param).c_str());
		}
	}

	const auto drivers_param = input.named_parameters.find("allowed_drivers");
	if (drivers_param != input.named_parameters.end()) {
		for (auto &param : ListValue::GetChildren(drivers_param->second)) {
			result->dataset_drivers.AddString(StringValue::Get(param).c_str());
		}
	}

	const auto siblings_params = input.named_parameters.find("sibling_files");
	if (siblings_params != input.named_parameters.end()) {
		for (auto &param : ListValue::GetChildren(siblings_params->second)) {
			result->dataset_sibling.AddString(file_prefix.AddPrefix(StringValue::Get(param)).c_str());
		}
	}

	const auto keep_wkb_param = input.named_parameters.find("keep_wkb");
	if (keep_wkb_param != input.named_parameters.end()) {
		result->keep_wkb = BooleanValue::Get(keep_wkb_param->second);
	}

	// Set additional default GDAL default options

	// This for OSM, but we don't know if we are reading OSM until we open the dataset, so just always set it for now.
	//result->dataset_options.AddString("INTERLEAVED_READING=YES");

	// This is so taht we dont have to deal with chunking ourselves, let GDAL do it for us
	result->layer_options.AddString(StringUtil::Format("MAX_FEATURES_IN_BATCH=%d", STANDARD_VECTOR_SIZE).c_str());

	// We always want GeoArrow geometry which DuckDB knows how to convert to GEOMETRY type, unless `keep_wkb` is set
	if (!result->keep_wkb) {
		result->layer_options.AddString("GEOMETRY_METADATA_ENCODING=GEOARROW");
	}

	// Open the dataset and get the Arrow schema
	const auto dataset = GDALOpenEx(result->gdal_file_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
	                                result->dataset_drivers, result->dataset_options, result->dataset_sibling);

	if (!dataset) {
		ThrowGDALError(StringUtil::Format("Could not open GDAL dataset at: %s", result->real_file_path));
	}

	ArrowSchema schema;
	ArrowArrayStream stream;

	try {

		const auto layer_count = GDALDatasetGetLayerCount(dataset);
		if (layer_count <= 0) {
			throw IOException("GDAL dataset contains no layers at: %s", result->real_file_path);
		}

		// Find layer
		const auto layer_param = input.named_parameters.find("layer");

		if (layer_param != input.named_parameters.end()) {
			if (layer_param->second.type() == LogicalType::INTEGER) {
				// Find layer by index
				const auto layer_idx = IntegerValue::Get(layer_param->second);
				if (layer_idx < 0) {
					throw BinderException("Layer index must be positive");
				}
				if (layer_idx > layer_count) {
					throw BinderException(
					    StringUtil::Format("Layer index out of range (%s > %s)", layer_idx, layer_count));
				}
				result->layer_idx = layer_idx;
			} else if (layer_param->second.type() == LogicalType::VARCHAR) {
				// Find layer by name
				const auto &layer_name = StringValue::Get(layer_param->second);
				auto found = false;
				for (int i = 0; i < layer_count; i++) {
					const auto layer = GDALDatasetGetLayer(dataset, i);
					if (!layer) {
						continue;
					}
					if (OGR_L_GetName(layer) == layer_name) {
						result->layer_idx = i;
						found = true;
						break;
					}
				}
				if (!found) {
					throw BinderException("Could not find layer with name: %s", layer_name);
				}
			}
		}

		// Get the layer by index
		const auto layer = GDALDatasetGetLayer(dataset, result->layer_idx);
		if (!layer) {
			throw IOException("Could not get GDAL layer at: %s", result->real_file_path);
		}

		// Estimate cardinality
		result->estimated_cardinality = OGR_L_GetFeatureCount(layer, 0);

		// Get extent (Only if spatial filter is not pushed down!)
		if (OGR_L_GetExtent(layer, &result->layer_extent, 0) == OGRERR_NONE) {
			result->has_extent = true;
		}

		// Check if fast spatial filtering is available
		if (OGR_L_TestCapability(layer, OLCFastSpatialFilter)) {
			result->can_filter = true;
		}

		// Get the layer geometry type if available
		result->layer_type = OGR_L_GetGeomType(layer);

		// Only suppress the FID if the layer already exposes it as a regular attribute field
		const auto fid_col = OGR_L_GetFIDColumn(layer);
		if (fid_col && strcmp(fid_col, "") != 0) {
			const auto layer_defn = OGR_L_GetLayerDefn(layer);
			if (OGR_FD_GetFieldIndex(layer_defn, fid_col) >= 0) {
				result->layer_options.AddString("INCLUDE_FID=NO");
			}
		}

		const auto geom_col_name = OGR_L_GetGeometryColumn(layer);

		// Get the arrow stream
		if (!OGR_L_GetArrowStream(layer, &stream, result->layer_options.List())) {
			ThrowGDALError(StringUtil::Format("Could not get GDAL Arrow stream at: %s", result->real_file_path));
		}

		// And the schema
		if (stream.get_schema(&stream, &schema) != 0) {
			throw IOException("Could not get GDAL Arrow schema at: %s", result->real_file_path);
		}

		// Convert Arrow schema to DuckDB types
		for (int64_t i = 0; i < schema.n_children; i++) {
			auto &child_schema = *schema.children[i];
			const auto gdal_type = ArrowType::GetTypeFromSchema(ctx, child_schema);
			auto duck_type = gdal_type->GetDuckType();

			// Track geometry columns to compute stats later
			if (duck_type.id() == LogicalTypeId::GEOMETRY) {
				result->geometry_columns.insert(i);
			}

			if (geom_col_name && (strcmp(geom_col_name, "") == 0) && (strcmp(child_schema.name, "wkb_geometry") == 0) &&
			    !result->keep_wkb) {
				// Rename the geometry column to "geom" unless keep_wkb is set
				col_names.push_back("geom");
			} else {
				col_names.emplace_back(child_schema.name);
			}

			if (duck_type.id() != LogicalTypeId::GEOMETRY) {
				col_types.push_back(std::move(duck_type));
				continue;
			}

			if (!GeoType::HasCRS(duck_type)) {
				col_types.push_back(std::move(duck_type));
				continue;
			}

			// Try to identify the CRS
			const auto &gdal_crs = GeoType::GetCRS(duck_type);

			// FIXME: GDAL should be able to do this on its own, but somehow
			// OGRSpatialReference::FindBestMatch() dont work

			string auth_name;
			string auth_code;
			if (IdentifyProjCRS(gdal_crs.GetDefinition().c_str(), auth_name, auth_code)) {
				// If we can identify the CRS using PROJ, we can be pretty sure that DuckDB will recognize it.
				auto authcode = auth_name + ":" + auth_code;
				auto duck_crs = CoordinateReferenceSystem::TryIdentify(ctx, authcode);
				if (duck_crs) {
					col_types.push_back(LogicalType::GEOMETRY(*duck_crs));
					continue;
				}
			}

			// Otherwise just pass on what we got
			col_types.push_back(std::move(duck_type));
		}

	} catch (...) {
		// Release stream, schema and dataset
		if (schema.release) {
			schema.release(&schema);
		}
		if (stream.release) {
			stream.release(&stream);
		}
		if (dataset) {
			GDALClose(dataset);
		}
		// Re-throw exception
		throw;
	}

	if (schema.release) {
		schema.release(&schema);
	}
	if (stream.release) {
		stream.release(&stream);
	}
	if (dataset) {
		GDALClose(dataset);
	}

	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// FILTER (EXPRESSION) PUSHDOWN
//----------------------------------------------------------------------------------------------------------------------
auto Pushdown(ClientContext &context, LogicalGet &get, FunctionData *bind_data, vector<unique_ptr<Expression>> &filters)
    -> void {

	auto &bdata = bind_data->Cast<BindData>();

	if (!bdata.can_filter) {
		return;
	}

	if (bdata.geometry_columns.size() != 1) {
		return; // Only optimize if there is a single geometry column
	}

	optional_idx geom_filter_idx = optional_idx::Invalid();

	for (idx_t expr_idx = 0; expr_idx < filters.size(); expr_idx++) {
		const auto &expr = filters[expr_idx];

		if (expr->GetExpressionType() != ExpressionType::BOUND_FUNCTION) {
			continue;
		}
		if (expr->GetReturnType() != LogicalType::BOOLEAN) {
			continue;
		}
		const auto &func = expr->Cast<BoundFunctionExpression>();
		if (func.GetChildren().size() != 2) {
			continue;
		}

		if (func.GetChildren()[0]->GetReturnType().id() != LogicalTypeId::GEOMETRY ||
		    func.GetChildren()[1]->GetReturnType().id() != LogicalTypeId::GEOMETRY) {
			continue;
		}

		// The set of geometry predicates that can be optimized using the bounding box
		static constexpr const char *geometry_predicates[12] = {
		    "ST_Equals",   "ST_Intersects", "ST_Touches",   "ST_Crosses",          "ST_Within", "ST_Contains",
		    "ST_Overlaps", "ST_Covers",     "ST_CoveredBy", "ST_ContainsProperly", "&&",        "ST_Intersects_Extent"};

		auto found = false;
		for (const auto &name : geometry_predicates) {
			if (StringUtil::CIEquals(func.Function().GetName().c_str(), name)) {
				found = true;
				break;
			}
		}
		if (!found) {
			// Not a geometry predicate we can optimize
			continue;
		}

		const auto lhs_kind = func.GetChildren()[0]->GetExpressionType();
		const auto rhs_kind = func.GetChildren()[1]->GetExpressionType();

		const auto lhs_is_const = lhs_kind == ExpressionType::VALUE_CONSTANT;
		const auto rhs_is_const = rhs_kind == ExpressionType::VALUE_CONSTANT;

		if (lhs_is_const == rhs_is_const) {
			// Both sides are constant or both sides are column refs
			continue;
		}

		auto &constant_expr = func.GetChildren()[lhs_is_const ? 0 : 1]->Cast<BoundConstantExpression>();
		auto &geometry_expr = func.GetChildren()[lhs_is_const ? 1 : 0];

		auto found_geom_expr = false;
		auto multi_geom_expr = false;
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    *geometry_expr, [&](const BoundColumnRefExpression &ref) {
			    if (found_geom_expr) {
				    multi_geom_expr = true;
				    return;
			    }
			    if (ref.Binding().table_index != get.table_index) {
				    // Not from the same table
				    return;
			    }

			    const auto &col_ids = get.GetColumnIds();
			    const auto &col = col_ids[ref.Binding().column_index];

			    if (!col.HasPrimaryIndex()) {
				    return;
			    }
			    if (col.GetPrimaryIndex() != *bdata.geometry_columns.begin()) {
				    return;
			    }
			    found_geom_expr = true;
		    });

		if (!found_geom_expr || multi_geom_expr) {
			// No geometry column ref found or multiple geometry refs found
			continue;
		}

		if (constant_expr.GetValue().type().id() != LogicalTypeId::GEOMETRY) {
			// Constant is not geometry
			continue;
		}
		if (constant_expr.GetValue().IsNull()) {
			// Constant is NULL
			continue;
		}
		if (geometry_expr->GetReturnType().id() != LogicalTypeId::GEOMETRY) {
			// Not the geometry column
			continue;
		}

		auto geom_extent = GeometryExtent::Empty();
		auto geom_binary = string_t(StringValue::Get(constant_expr.GetValue()));

		if (Geometry::GetExtent(geom_binary, geom_extent)) {
			bdata.has_filter = true;
			bdata.layer_filter.MinX = geom_extent.x_min;
			bdata.layer_filter.MinY = geom_extent.y_min;
			bdata.layer_filter.MaxX = geom_extent.x_max;
			bdata.layer_filter.MaxY = geom_extent.y_max;
		}

		// Set the index so we can remove it later
		// We can __ONLY__ do this if the filter predicate is "&&" or "st_intersects_extent"
		// as other predicates may require exact geometry evaluation, the filter cannot be fully removed
		for (auto &name : {"&&", "ST_Intersects_Extent"}) {
			if (StringUtil::CIEquals(func.Function().GetName().c_str(), name)) {
				geom_filter_idx = expr_idx;
				break;
			}
		}

		break;
	}

	if (geom_filter_idx != optional_idx::Invalid()) {
		// Remove the filter from the list
		filters.erase_at(geom_filter_idx.GetIndex());
	}
}

//----------------------------------------------------------------------------------------------------------------------
// GLOBAL STATE
//----------------------------------------------------------------------------------------------------------------------
class GlobalState final : public GlobalTableFunctionState {
public:
	~GlobalState() override {
		if (stream.release) {
			stream.release(&stream);
		}

		if (schema.release) {
			schema.release(&schema);
		}

		if (dataset) {
			GDALClose(dataset);
			dataset = nullptr;
		}
	}

	GDALDatasetH dataset;
	CPLStringList layer_options;
	OGRLayerH layer;
	ArrowArrayStream stream;
	ArrowSchema schema;
	vector<unique_ptr<ArrowType>> col_types;
	atomic<idx_t> features_read = {0};
};

auto InitGlobal(ClientContext &context, TableFunctionInitInput &input) -> unique_ptr<GlobalTableFunctionState> {
	auto &bdata = input.bind_data->Cast<BindData>();

	const auto dataset = GDALOpenEx(bdata.gdal_file_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
	                                bdata.dataset_drivers, bdata.dataset_options, bdata.dataset_sibling);

	if (!dataset) {
		ThrowGDALError(StringUtil::Format("Could not open GDAL dataset at: %s", bdata.real_file_path));
	}

	auto result = make_uniq<GlobalState>();
	result->dataset = dataset;
	result->layer_options = bdata.layer_options;

	const auto driver = GDALGetDatasetDriver(dataset);
	if (strcmp(GDALGetDriverShortName(driver), "OSM") != 0) {
		// Get the layer by index
		result->layer = GDALDatasetGetLayer(dataset, bdata.layer_idx);
	} else {
		// Special case for OSM, which requires sequential reading of layers
		const auto layer_count = GDALDatasetGetLayerCount(dataset);
		for (int i = 0; i < layer_count; i++) {
			result->layer = GDALDatasetGetLayer(dataset, i);
			if (i == bdata.layer_idx) {
				// desired layer found
				break;
			}

			// else scan through and empty the layer
			OGRFeatureH feature;
			while ((feature = OGR_L_GetNextFeature(result->layer)) != nullptr) {
				OGR_F_Destroy(feature);
			}
		}
	}

	// Set the filter, if we got one
	if (bdata.has_filter) {
		OGR_L_SetSpatialFilterRect(result->layer, bdata.layer_filter.MinX, bdata.layer_filter.MinY,
		                           bdata.layer_filter.MaxX, bdata.layer_filter.MaxY);
	}

	CPLStringList layer_options;
	layer_options.AddString(StringUtil::Format("MAX_FEATURES_IN_BATCH=%d", STANDARD_VECTOR_SIZE).data());
	layer_options.AddString("GEOMETRY_METADATA_ENCODING=GEOARROW");

	// Open the Arrow stream
	if (!OGR_L_GetArrowStream(result->layer, &result->stream, result->layer_options.List())) {
		ThrowGDALError("Could not get GDAL Arrow stream");
	}

	if (result->stream.get_schema(&result->stream, &result->schema) != 0) {
		throw IOException("Could not get GDAL Arrow schema");
	}

	// Store the column types
	for (int64_t i = 0; i < result->schema.n_children; i++) {
		auto &child_schema = *result->schema.children[i];
		result->col_types.push_back(ArrowType::GetTypeFromSchema(context, child_schema));
	}

	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// SCAN
//----------------------------------------------------------------------------------------------------------------------
void Scan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<GlobalState>();

	ArrowArray arrow_array;
	if (state.stream.get_next(&state.stream, &arrow_array) != 0 || arrow_array.release == nullptr) {
		// Finished reading
		output.SetCardinality(0);
		return;
	}

	// Now convert the Arrow array to DuckDB
	for (idx_t i = 0; i < arrow_array.n_children; i++) {
		auto &arr = *arrow_array.children[i];
		auto &vec = output.data[i];

		auto &arrow_type = *state.col_types[i];
		auto array_state = ArrowArrayScanState(context);

		// We need to make sure that our chunk will hold the ownership
		array_state.owned_data = make_shared_ptr<ArrowArrayWrapper>();
		array_state.owned_data->arrow_array = arrow_array;

		// We set it to nullptr to effectively transfer the ownership
		arrow_array.release = nullptr;

		switch (arrow_type.GetPhysicalType()) {
		case ArrowArrayPhysicalType::DICTIONARY_ENCODED:
			ArrowToDuckDBConversion::ColumnArrowToDuckDBDictionary(vec, arr, 0, array_state, arrow_array.length,
			                                                       arrow_type);
			break;
		case ArrowArrayPhysicalType::RUN_END_ENCODED:
			ArrowToDuckDBConversion::ColumnArrowToDuckDBRunEndEncoded(vec, arr, 0, array_state, arrow_array.length,
			                                                          arrow_type);
			break;
		case ArrowArrayPhysicalType::DEFAULT:
			ArrowToDuckDBConversion::SetValidityMask(vec, arr, 0, arrow_array.length, arrow_array.offset, -1);
			ArrowToDuckDBConversion::ColumnArrowToDuckDB(vec, arr, 0, array_state, arrow_array.length, arrow_type);
			break;
		default:
			throw NotImplementedException("ArrowArrayPhysicalType not recognized");
		}
		FlatVector::SetSize(vec, count_t(arrow_array.length));
	}

	state.features_read += arrow_array.length;
	output.SetCardinality(arrow_array.length);
}

//------------------------------------------------------------------------------------------------------------------
// CARDINALITY
//------------------------------------------------------------------------------------------------------------------
auto Cardinality(ClientContext &context, const FunctionData *data) -> unique_ptr<NodeStatistics> {
	auto &bdata = data->Cast<BindData>();
	auto result = make_uniq<NodeStatistics>();

	if (bdata.estimated_cardinality > -1) {
		result->has_estimated_cardinality = true;
		result->estimated_cardinality = bdata.estimated_cardinality;
		result->has_max_cardinality = true;
		result->max_cardinality = bdata.estimated_cardinality;
	}

	return result;
}

//----------------------------------------------------------------------------------------------------------------------
// STATISTICS
//----------------------------------------------------------------------------------------------------------------------
auto Statistics(ClientContext &context, const FunctionData *bind_data, column_t column_index)
    -> unique_ptr<BaseStatistics> {

	auto &bdata = bind_data->Cast<BindData>();

	// If we have an extent, and the column is a geometry column, we can provide min/max stats
	if (bdata.has_extent) {

		// Check if this is the only geometry column
		const auto is_geom_col = bdata.geometry_columns.find(column_index) != bdata.geometry_columns.end();
		const auto is_only_one = bdata.geometry_columns.size() == 1;
		const auto has_stats = bdata.has_extent || bdata.layer_type != wkbUnknown;

		if (is_geom_col && is_only_one && has_stats) {
			auto stats = GeometryStats::CreateUnknown(LogicalType::GEOMETRY());

			if (bdata.has_extent) {
				auto &extent = GeometryStats::GetExtent(stats);
				extent.x_min = bdata.layer_extent.MinX;
				extent.x_max = bdata.layer_extent.MaxX;
				extent.y_min = bdata.layer_extent.MinY;
				extent.y_max = bdata.layer_extent.MaxY;
			}

			const auto geom_type = bdata.layer_type % 1000;
			const auto vert_type = bdata.layer_type / 1000;

			if ((geom_type >= 1) && (geom_type <= 7) && (vert_type >= 0) && (vert_type <= 3)) {
				auto &types = GeometryStats::GetTypes(stats);
				types.Clear();
				types.AddWKBType(static_cast<int32_t>(geom_type));
			}

			return stats.ToUnique();
		}
	}

	return nullptr;
}

//----------------------------------------------------------------------------------------------------------------------
// PROGRESS
//----------------------------------------------------------------------------------------------------------------------
auto Progress(ClientContext &context, const FunctionData *b_data, const GlobalTableFunctionState *g_state) -> double {
	auto &bdata = b_data->Cast<BindData>();
	auto &gstate = g_state->Cast<GlobalState>();

	if (bdata.estimated_cardinality < 0) {
		return -1.0;
	}
	if (bdata.estimated_cardinality == 0) {
		return 0.0;
	}

	const auto count = static_cast<double>(gstate.features_read.load());
	const auto total = static_cast<double>(bdata.estimated_cardinality);

	return MinValue(100.0 * (count / total), 100.0);
}

//------------------------------------------------------------------------------------------------------------------
// REPLACEMENT SCAN
//------------------------------------------------------------------------------------------------------------------
auto ReplacementScan(ClientContext &, ReplacementScanInput &input, optional_ptr<ReplacementScanData>)
    -> unique_ptr<TableRef> {
	auto &table_name = input.table_name;
	auto lower_name = StringUtil::Lower(table_name);
	// Check if the table name ends with some common geospatial file extensions
	if (StringUtil::EndsWith(lower_name, ".gpkg") || StringUtil::EndsWith(lower_name, ".fgb")) {

		auto table_function = make_uniq<TableFunctionRef>();
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
		table_function->function = make_uniq<FunctionExpression>("ST_Read", std::move(children));
		return std::move(table_function);
	}
	// else not something we can replace
	return nullptr;
}

//------------------------------------------------------------------------------------------------------------------
// TO STRING
//------------------------------------------------------------------------------------------------------------------
auto ToString(TableFunctionToStringInput &input) -> InsertionOrderPreservingMap<string> {
	auto &bdata = input.bind_data->Cast<BindData>();
	InsertionOrderPreservingMap<string> result;
	result.insert("Layer", to_string(bdata.layer_idx));

	if (bdata.has_filter) {
		result.insert("Filter (MinX, MinY, MaxX, MaxY)",
		              StringUtil::Format("(%f, %f, %f, %f)", bdata.layer_filter.MinX, bdata.layer_filter.MinY,
		                                 bdata.layer_filter.MaxX, bdata.layer_filter.MaxY));
	}
	return result;
}

//----------------------------------------------------------------------------------------------------------------------
// REGISTER
//----------------------------------------------------------------------------------------------------------------------
static constexpr auto DOCUMENTATION = R"(
	    Read and import a variety of geospatial file formats using the GDAL library.

	    The `ST_Read` table function is based on the [GDAL](https://gdal.org/index.html) translator library and enables reading spatial data from a variety of geospatial vector file formats as if they were DuckDB tables.

	    > See [ST_Drivers](#st_drivers) for a list of supported file formats and drivers.

	    Except for the `path` parameter, all parameters are optional.

	    | Parameter | Type | Description |
	    | --------- | -----| ----------- |
	    | `path` | VARCHAR | The path to the file to read. Mandatory |
	    | `sequential_layer_scan` | BOOLEAN | If set to true, the table function will scan through all layers sequentially and return the first layer that matches the given layer name. This is required for some drivers to work properly, e.g., the OSM driver. |
	    | `spatial_filter` | WKB_BLOB | If set to a WKB blob, the table function will only return rows that intersect with the given WKB geometry. Some drivers may support efficient spatial filtering natively, in which case it will be pushed down. Otherwise the filtering is done by GDAL which may be much slower. |
	    | `open_options` | VARCHAR[] | A list of key-value pairs that are passed to the GDAL driver to control the opening of the file. E.g., the GeoJSON driver supports a FLATTEN_NESTED_ATTRIBUTES=YES option to flatten nested attributes. |
	    | `layer` | VARCHAR | The name of the layer to read from the file. If NULL, the first layer is returned. Can also be a layer index (starting at 0). |
	    | `allowed_drivers` | VARCHAR[] | A list of GDAL driver names that are allowed to be used to open the file. If empty, all drivers are allowed. |
	    | `sibling_files` | VARCHAR[] | A list of sibling files that are required to open the file. E.g., the ESRI Shapefile driver requires a .shx file to be present. Although most of the time these can be discovered automatically. |
	    | `spatial_filter_box` | BOX_2D | If set to a BOX_2D, the table function will only return rows that intersect with the given bounding box. Similar to spatial_filter. |
	    | `keep_wkb` | BOOLEAN | If set, the table function will return geometries in a wkb_geometry column with the type WKB_BLOB (which can be cast to BLOB) instead of GEOMETRY. This is useful if you want to use DuckDB with more exotic geometry subtypes that DuckDB spatial doesn't support representing in the GEOMETRY type yet. |

	    Note that GDAL is single-threaded, so this table function will not be able to make full use of parallelism.

	    By using `ST_Read`, the spatial extension also provides “replacement scans” for common geospatial file formats, allowing you to query files of these formats as if they were tables directly.

	    ```sql
	    SELECT * FROM './path/to/some/shapefile/dataset.shp';
	    ```

	    In practice this is just syntax-sugar for calling ST_Read, so there is no difference in performance. If you want to pass additional options, you should use the ST_Read table function directly.

	    The following formats are currently recognized by their file extension:

	    | Format | Extension |
	    | ------ | --------- |
	    | ESRI ShapeFile | .shp |
	    | GeoPackage | .gpkg |
	    | FlatGeoBuf | .fgb |
	)";

static constexpr auto EXAMPLE = R"(
		-- Read a Shapefile
		SELECT * FROM ST_Read('some/file/path/filename.shp');

		-- Read a GeoJSON file
		CREATE TABLE my_geojson_table AS SELECT * FROM ST_Read('some/file/path/filename.json');
	)";

void Register(ExtensionLoader &loader) {
	TableFunction read_func("ST_Read", {LogicalType::VARCHAR}, Scan, Bind, InitGlobal);
	read_func.cardinality = Cardinality;
	read_func.statistics = Statistics;
	read_func.table_scan_progress = Progress;
	read_func.pushdown_complex_filter = Pushdown;
	read_func.to_string = ToString;

	read_func.named_parameters["open_options"] = LogicalType::LIST(LogicalType::VARCHAR);
	read_func.named_parameters["allowed_drivers"] = LogicalType::LIST(LogicalType::VARCHAR);
	read_func.named_parameters["sibling_files"] = LogicalType::LIST(LogicalType::VARCHAR);
	read_func.named_parameters["layer"] = LogicalType::VARCHAR;
	read_func.named_parameters["max_batch_size"] = LogicalType::INTEGER;
	read_func.named_parameters["keep_wkb"] = LogicalType::BOOLEAN;
	read_func.parallelism = TableFunctionParallelism::SEQUENTIAL;

	loader.RegisterFunction(read_func);

	InsertionOrderPreservingMap<string> tags;
	tags.insert("ext", "spatial");
	FunctionBuilder::AddTableFunctionDocs(loader, "ST_Read", DOCUMENTATION, EXAMPLE, tags);

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(ReplacementScan);
}

} // namespace gdal_read
//======================================================================================================================
// GDAL COPY
//======================================================================================================================
namespace gdal_copy {

//----------------------------------------------------------------------------------------------------------------------
// Bind
//----------------------------------------------------------------------------------------------------------------------
class BindData final : public TableFunctionData {
public:
	//string gdal_file_path;
	//string real_file_path;
	string driver_name;
	string layer_name;

	CPLStringList driver_options;
	CPLStringList layer_options;

	string target_srs;

	bool always_xy = false;

	OGRwkbGeometryType geometry_type;

	// Arrow info
	ClientProperties props;
	ArrowSchema schema;
	unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_type_cast;

	BindData() : geometry_type(wkbUnknown) {
		schema.release = nullptr;
	}

	// Move constructor
	BindData(BindData &&other) noexcept
	    : driver_name(std::move(other.driver_name)), layer_name(std::move(other.layer_name)),
	      driver_options(std::move(other.driver_options)), layer_options(std::move(other.layer_options)),
	      target_srs(std::move(other.target_srs)), always_xy(other.always_xy), geometry_type(other.geometry_type),
	      props(std::move(other.props)), schema(other.schema),
	      extension_type_cast(std::move(other.extension_type_cast)) {
		other.schema.release = nullptr;
	}

	// Move assignment operator
	BindData &operator=(BindData &&other) noexcept {
		std::swap(driver_name, other.driver_name);
		std::swap(layer_name, other.layer_name);
		std::swap(driver_options, other.driver_options);
		std::swap(layer_options, other.layer_options);
		std::swap(target_srs, other.target_srs);
		std::swap(always_xy, other.always_xy);
		std::swap(geometry_type, other.geometry_type);
		std::swap(props, other.props);
		std::swap(schema, other.schema);
		std::swap(extension_type_cast, other.extension_type_cast);
		return *this;
	}

	~BindData() override {
		if (schema.release) {
			schema.release(&schema);
		}
	}
};

bool MatchOption(const char *name, const pair<const Identifier, vector<Value>> &option, bool list = false) {
	if (option.first == name) {
		if (option.second.empty()) {
			throw BinderException("GDAL COPY option '%s' requires a value", name);
		}
		if (!list) {
			if (option.second.size() != 1) {
				throw BinderException("GDAL COPY option '%s' only accepts a single value", name);
			}
			if (option.second.back().type().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("GDAL COPY option '%s' must be a string", name);
			}
		} else {
			for (auto &val : option.second) {
				if (val.type().id() != LogicalTypeId::VARCHAR) {
					throw BinderException("GDAL COPY option '%s' must be a list of strings", name);
				}
			}
		}
		return true;
	}
	return false;
}

auto Bind(ClientContext &context, CopyFunctionBindInput &input, const vector<Identifier> &names,
          const vector<LogicalType> &sql_types) -> unique_ptr<FunctionData> {
	auto result = make_uniq<BindData>();

	// Set file pat
	const auto &file_path = input.info.file_path;

	// Parse options
	for (auto &option : input.info.options) {

		if (MatchOption("DRIVER", option)) {
			result->driver_name = option.second.back().GetValue<string>();
			continue;
		}

		if (MatchOption("LAYER_NAME", option)) {
			result->layer_name = option.second.back().GetValue<string>();
			continue;
		}

		if (MatchOption("SRS", option) || MatchOption("CRS", option)) {
			result->target_srs = option.second.back().GetValue<string>();
			continue;
		}

		if (MatchOption("GEOMETRY_TYPE", option)) {
			auto type = option.second.back().GetValue<string>();
			if (StringUtil::CIEquals(type, "POINT")) {
				result->geometry_type = wkbPoint;
			} else if (StringUtil::CIEquals(type, "LINESTRING")) {
				result->geometry_type = wkbLineString;
			} else if (StringUtil::CIEquals(type, "POLYGON")) {
				result->geometry_type = wkbPolygon;
			} else if (StringUtil::CIEquals(type, "MULTIPOINT")) {
				result->geometry_type = wkbMultiPoint;
			} else if (StringUtil::CIEquals(type, "MULTILINESTRING")) {
				result->geometry_type = wkbMultiLineString;
			} else if (StringUtil::CIEquals(type, "MULTIPOLYGON")) {
				result->geometry_type = wkbMultiPolygon;
			} else if (StringUtil::CIEquals(type, "GEOMETRYCOLLECTION")) {
				result->geometry_type = wkbGeometryCollection;
			} else {
				throw BinderException("Unsupported GEOMETRY_TYPE: '%s'", type);
			}
			continue;
		}

		if (MatchOption("LAYER_CREATION_OPTIONS", option, true)) {
			for (auto &val : option.second) {
				result->layer_options.AddString(val.GetValue<string>().c_str());
			}
			continue;
		}

		if (MatchOption("DATASET_CREATION_OPTIONS", option, true)) {
			for (auto &val : option.second) {
				result->driver_options.AddString(val.GetValue<string>().c_str());
			}
			continue;
		}

		throw BinderException("Unknown GDAL COPY option: '%s'", option.first);
	}

	// Read the global geometry_always_xy setting to control axis order
	{
		bool is_set = false;
		result->always_xy = SpatialSettings::AlwaysXY(context, is_set);

		if (!is_set) {
			constexpr auto message =
			    "GDAL COPY TO with an SRS is sensitive to the coordinate axis order.\n"
			    "DuckDB GEOMETRY uses [EASTING, NORTHING] (= LONGITUDE, LATITUDE) internally.\n"
			    "Without 'geometry_always_xy', GDAL may interpret coordinates in authority-defined order\n"
			    "(e.g., [LATITUDE, LONGITUDE] for EPSG:4326), causing incorrect output in KML and similar formats.\n"
			    "To avoid unexpected results:\n"
			    " * 'SET geometry_always_xy = true' to always use [EASTING, NORTHING] (recommended)\n"
			    " * 'SET geometry_always_xy = false' to use authority-defined axis order";

			auto &logger = Logger::Get(context);
			logger.WriteLog("Spatial", LogLevel::LOG_WARNING, message);
		}
	}

	// If no override SRS is set, we will use the SRS of the first geometry column
	if (result->target_srs.empty()) {
		for (auto &col : sql_types) {
			if (col.id() == LogicalTypeId::GEOMETRY && GeoType::HasCRS(col)) {
				result->target_srs = GeoType::GetCRS(col).GetDefinition();
				break;
			}
		}
	}

	// Check that options are valid
	if (result->driver_name.empty()) {
		throw BinderException("GDAL COPY option 'DRIVER' is required");
	}

	if (result->layer_name.empty()) {
		auto &fs = FileSystem::GetFileSystem(context);
		result->layer_name = fs.ExtractBaseName(file_path);
	}

	// Check the driver
	const auto driver = GDALGetDriverByName(result->driver_name.c_str());
	if (!driver) {
		throw BinderException("Could not find GDAL driver: " + result->driver_name);
	}

	// Try to get the file extension from the driver
	const auto file_ext = GDALGetMetadataItem(driver, GDAL_DMD_EXTENSIONS, nullptr);
	if (file_ext) {
		input.file_extension = file_ext;
	} else {
		const auto file_exts = GDALGetMetadataItem(driver, GDAL_DMD_EXTENSIONS, nullptr);
		const auto exts = StringUtil::Split(file_exts, ' ');
		if (!exts.empty()) {
			input.file_extension = exts[0];
		}
	}

	// Driver-specific checks
	if (result->driver_name == "OpenFileGDB" && result->geometry_type == wkbUnknown) {
		throw BinderException("OpenFileGDB requires 'GEOMETRY_TYPE' parameter to be set when writing!");
	}

	// Setup arrow schema
	result->props = context.GetClientProperties();
	result->extension_type_cast = duckdb::ArrowTypeExtensionData::GetExtensionTypes(context, sql_types);
	ArrowConverter::ToArrowSchema(&result->schema, sql_types, IdentifiersToStrings(names), result->props);

	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Global State
//----------------------------------------------------------------------------------------------------------------------
class GlobalState final : public GlobalFunctionData {
public:
	GlobalState() {
		dataset = nullptr;
		layer = nullptr;
		srs = nullptr;
	}

	// Move constructor
	GlobalState(GlobalState &&other) noexcept : dataset(other.dataset), layer(other.layer), srs(other.srs) {
		other.dataset = nullptr;
		other.layer = nullptr;
		other.srs = nullptr;
	}

	// Move assignment operator
	GlobalState &operator=(GlobalState &&other) noexcept {
		std::swap(dataset, other.dataset);
		std::swap(layer, other.layer);
		std::swap(srs, other.srs);
		return *this;
	}

	~GlobalState() override {
		if (dataset) {
			GDALClose(dataset);
			dataset = nullptr;
		}
		if (srs) {
			OSRRelease(srs);
			srs = nullptr;
		}
	}

	mutex lock;
	GDALDatasetH dataset;
	OGRLayerH layer;
	OGRSpatialReferenceH srs;
};

auto InitGlobal(ClientContext &context, FunctionData &bdata_p, const string &real_file_path)
    -> unique_ptr<GlobalFunctionData> {
	auto &bdata = bdata_p.Cast<BindData>();
	auto result = make_uniq<GlobalState>();

	const auto driver = GDALGetDriverByName(bdata.driver_name.c_str());
	if (!driver) {
		throw InvalidInputException("Could not find GDAL driver: " + bdata.driver_name);
	}

	const auto &file_prefix = DuckDBFileSystemPrefix::GetOrCreate(context);
	const auto gdal_file_path = file_prefix.AddPrefix(real_file_path);

	// Create Dataset
	result->dataset = GDALCreate(driver, gdal_file_path.c_str(), 0, 0, 0, GDT_Unknown, bdata.driver_options);
	if (!result->dataset) {
		ThrowGDALError("Could not create GDAL dataset at: " + real_file_path);
	}

	if (!bdata.target_srs.empty()) {
		// Make a new spatial reference object, and set it from the user input
		result->srs = OSRNewSpatialReference(nullptr);

		// Try get the complete SRS from duckdb
		auto converted =
		    CoordinateReferenceSystem::TryConvert(context, bdata.target_srs, CoordinateReferenceSystemType::PROJJSON);
		if (!converted) {
			converted = CoordinateReferenceSystem::TryConvert(context, bdata.target_srs,
			                                                  CoordinateReferenceSystemType::WKT2_2019);
			if (!converted) {
				converted = CoordinateReferenceSystem::TryConvert(context, bdata.target_srs,
				                                                  CoordinateReferenceSystemType::AUTH_CODE);
			}
		}

		if (converted) {
			OSRSetFromUserInput(result->srs, converted->GetDefinition().c_str());
		} else {
			// Try to set it directly from the user input, in case it's in a format that duckdb doesn't recognize but GDAL does
			OSRSetFromUserInput(result->srs, bdata.target_srs.c_str());
		}

		if (bdata.always_xy) {
			// Controlled by the global 'geometry_always_xy' setting.
			// DuckDB GEOMETRY uses traditional GIS axis order (lon, lat).
			// Without this, GDAL 3.x defaults to authority-compliant order for
			// EPSG:4326 (lat, lon), causing KML and other drivers to reject valid
			// longitudes > 90 as out-of-range latitudes.
			OSRSetAxisMappingStrategy(result->srs, OAMS_TRADITIONAL_GIS_ORDER);
		}
	}

	// Create Layer
	result->layer = GDALDatasetCreateLayer(result->dataset, bdata.layer_name.c_str(), result->srs, bdata.geometry_type,
	                                       bdata.layer_options);

	if (!result->layer) {
		ThrowGDALError("Could not create GDAL layer in dataset at: " + real_file_path);
	}

	// Create fields for all children
	auto geometry_field_count = 0;
	for (auto i = 0; i < bdata.schema.n_children; i++) {
		const auto child_schema = bdata.schema.children[i];

		// Check if this is a geometry field
		if (child_schema->metadata != nullptr) {
			// TODO: Look for arrow metadata!
			geometry_field_count++;
			if (geometry_field_count > 1) {
				throw NotImplementedException("Multiple geometry fields not supported yet");
			}
			continue;
		}

		// Check if this is the FID field
		if (strcmp(child_schema->name, OGRLayer::DEFAULT_ARROW_FID_NAME) == 0) {
			// Skip FID field
			continue;
		}

		// Register normal attribute
		if (!OGR_L_CreateFieldFromArrowSchema(result->layer, child_schema, nullptr)) {
			ThrowGDALError("Could not create field in GDAL layer for column: " + string(child_schema->name));
		}
	}

	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Local State
//----------------------------------------------------------------------------------------------------------------------
class LocalState final : public LocalFunctionData {
public:
	LocalState() {
		array.release = nullptr;
	}

	// Move constructor
	LocalState(LocalState &&other) noexcept : array(other.array) {
		other.array.release = nullptr;
	}

	// Move assignment operator
	LocalState &operator=(LocalState &&other) noexcept {
		std::swap(array, other.array);
		return *this;
	}

	~LocalState() override {
		if (array.release) {
			array.release(&array);
			array.release = nullptr;
		}
	}
	ArrowArray array;
};

auto InitLocal(ExecutionContext &context, FunctionData &bind_data) -> unique_ptr<LocalFunctionData> {
	auto result = make_uniq<LocalState>();
	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Sink
//----------------------------------------------------------------------------------------------------------------------
void Sink(ExecutionContext &context, FunctionData &bdata_p, GlobalFunctionData &gstate_p, LocalFunctionData &lstate_p,
          DataChunk &input) {

	const auto &bdata = bdata_p.Cast<BindData>();
	auto &gstate = gstate_p.Cast<GlobalState>();
	auto &lstate = lstate_p.Cast<LocalState>();

	auto &arrow_array = lstate.array;
	auto &arrow_schema = bdata.schema;

	// Convert to Arrow array
	ArrowConverter::ToArrowArray(input, &arrow_array, bdata.props, bdata.extension_type_cast);

	// Sink the Arrow array into GDAL
	{
		// Lock
		lock_guard<mutex> guard(gstate.lock);

		if (!OGR_L_WriteArrowBatch(gstate.layer, &arrow_schema, &arrow_array, nullptr)) {
			ThrowGDALError("Could not write Arrow batch to GDAL layer");
		}
	}

	// Release the array
	if (arrow_array.release) {
		arrow_array.release(&arrow_array);
		arrow_array.release = nullptr;
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Combine
//----------------------------------------------------------------------------------------------------------------------
void Combine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
             LocalFunctionData &lstate) {
	// Nothing to do, we don't have any local state that needs to be merged
}

//----------------------------------------------------------------------------------------------------------------------
// Finalize
//----------------------------------------------------------------------------------------------------------------------
void Finalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate_p) {
	auto &gstate = gstate_p.Cast<GlobalState>();

	// Flush and close the dataset. If a flush fails we leave gstate.dataset set so the GlobalState destructor still
	// closes it during unwinding.
	if (GDALFlushCache(gstate.dataset) != CE_None) {
		ThrowGDALError("Could not flush GDAL dataset");
	}

	// GDALClose frees the dataset even on error, so clear the handle before checking to avoid a double close.
	const auto close_err = GDALClose(gstate.dataset);
	gstate.dataset = nullptr;
	if (close_err != CE_None) {
		ThrowGDALError("Could not close GDAL dataset");
	}
}

CopyFunctionExecutionMode Mode(bool preserve_insertion_order, bool use_batch_index) {
	// Parallel writes have limited utility since we still lock on each write to GDAL layer
	// But in theory we still benefit from the parallel conversion to Arrow arrays, and this also allows
	// the rest of the pipeline to be parallelized if we don't care about insertion order.
	return preserve_insertion_order ? CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE
	                                : CopyFunctionExecutionMode::PARALLEL_COPY_TO_FILE;
}

//----------------------------------------------------------------------------------------------------------------------
// Register
//----------------------------------------------------------------------------------------------------------------------
void Register(ExtensionLoader &loader) {
	CopyFunction info("GDAL");

	info.copy_to_bind = Bind;
	info.copy_to_initialize_local = InitLocal;
	info.copy_to_initialize_global = InitGlobal;
	info.copy_to_sink = Sink;
	info.copy_to_combine = Combine;
	info.copy_to_finalize = Finalize;
	info.execution_mode = Mode;
	info.extension = "gdal";

	loader.RegisterFunction(info);
}

} // namespace gdal_copy
} // namespace

//======================================================================================================================
// GDAL LIST
//======================================================================================================================
namespace gdal_list {

//----------------------------------------------------------------------------------------------------------------------
// Bind
//----------------------------------------------------------------------------------------------------------------------
class BindData final : public TableFunctionData {
public:
	idx_t driver_count;
};

auto Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &types, vector<Identifier> &names)
    -> unique_ptr<FunctionData> {

	types.emplace_back(LogicalType::VARCHAR);
	types.emplace_back(LogicalType::VARCHAR);
	types.emplace_back(LogicalType::BOOLEAN);
	types.emplace_back(LogicalType::BOOLEAN);
	types.emplace_back(LogicalType::BOOLEAN);
	types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("short_name");
	names.emplace_back("long_name");
	names.emplace_back("can_create");
	names.emplace_back("can_copy");
	names.emplace_back("can_open");
	names.emplace_back("help_url");

	auto result = make_uniq<BindData>();
	result->driver_count = GDALGetDriverCount();
	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Global State
//----------------------------------------------------------------------------------------------------------------------
class GlobalState final : public GlobalTableFunctionState {
public:
	idx_t current_idx;
};

auto InitGlobal(ClientContext &context, TableFunctionInitInput &input) -> unique_ptr<GlobalTableFunctionState> {
	auto result = make_uniq<GlobalState>();
	result->current_idx = 0;
	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Scan
//----------------------------------------------------------------------------------------------------------------------
void Scan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bdata = input.bind_data->Cast<BindData>();
	auto &gstate = input.global_state->Cast<GlobalState>();

	idx_t count = 0;

	const auto total_end = bdata.driver_count;
	const auto batch_end = gstate.current_idx + STANDARD_VECTOR_SIZE;
	const auto chunk_end = MinValue<idx_t>(batch_end, total_end);

	for (const auto next_idx = chunk_end; gstate.current_idx < next_idx; gstate.current_idx++) {
		auto driver = GDALGetDriver(static_cast<int>(gstate.current_idx));

		// Check if the driver is a vector driver
		if (GDALGetMetadataItem(driver, GDAL_DCAP_VECTOR, nullptr) == nullptr) {
			continue;
		}

		auto short_name = Value::CreateValue(GDALGetDriverShortName(driver));
		auto long_name = Value::CreateValue(GDALGetDriverLongName(driver));

		const char *create_flag = GDALGetMetadataItem(driver, GDAL_DCAP_CREATE, nullptr);
		auto create_value = Value::CreateValue(create_flag != nullptr);

		const char *copy_flag = GDALGetMetadataItem(driver, GDAL_DCAP_CREATECOPY, nullptr);
		auto copy_value = Value::CreateValue(copy_flag != nullptr);
		const char *open_flag = GDALGetMetadataItem(driver, GDAL_DCAP_OPEN, nullptr);
		auto open_value = Value::CreateValue(open_flag != nullptr);

		auto help_topic_flag = GDALGetDriverHelpTopic(driver);
		auto help_topic_value = help_topic_flag == nullptr
		                            ? Value(LogicalType::VARCHAR)
		                            : Value(StringUtil::Format("https://gdal.org/%s", help_topic_flag));

		output.data[0].SetValue(count, short_name);
		output.data[1].SetValue(count, long_name);
		output.data[2].SetValue(count, create_value);
		output.data[3].SetValue(count, copy_value);
		output.data[4].SetValue(count, open_value);
		output.data[5].SetValue(count, help_topic_value);
		count++;
	}
	output.SetChildCardinality(count);
}

//----------------------------------------------------------------------------------------------------------------------
// Register
//----------------------------------------------------------------------------------------------------------------------
static constexpr auto DESCRIPTION = R"(
	Returns the list of supported GDAL drivers and file formats

	Note that far from all of these drivers have been tested properly.
	Some may require additional options to be passed to work as expected.
	If you run into any issues please first consult the [consult the GDAL docs](https://gdal.org/drivers/vector/index.html).
)";

static constexpr auto EXAMPLE = R"(
	SELECT * FROM ST_Drivers();
)";

void Register(ExtensionLoader &loader) {
	TableFunction list_func("ST_Drivers", {}, Scan, Bind, InitGlobal);
	loader.RegisterFunction(list_func);

	InsertionOrderPreservingMap<string> tags;
	tags.insert("ext", "spatial");
	FunctionBuilder::AddTableFunctionDocs(loader, "ST_Drivers", DESCRIPTION, EXAMPLE, tags);
}

} // namespace gdal_list
//=====================================================================================================================
// GDAL META
//======================================================================================================================
namespace gdal_meta {

//----------------------------------------------------------------------------------------------------------------------
// Bind
//----------------------------------------------------------------------------------------------------------------------
class BindData final : public TableFunctionData {
public:
	vector<OpenFileInfo> files;
};

LogicalType GetGeometryFieldType() {
	return LogicalType::STRUCT({
	    {"name", LogicalType::VARCHAR},
	    {"type", LogicalType::VARCHAR},
	    {"nullable", LogicalType::BOOLEAN},
	    {"crs", LogicalType::STRUCT({
	                {"name", LogicalType::VARCHAR},
	                {"auth_name", LogicalType::VARCHAR},
	                {"auth_code", LogicalType::VARCHAR},
	                {"wkt", LogicalType::VARCHAR},
	                {"proj4", LogicalType::VARCHAR},
	                {"projjson", LogicalType::VARCHAR},
	            })},
	});
}

LogicalType GetStandardFieldType() {
	return LogicalType::STRUCT({
	    {"name", LogicalType::VARCHAR},
	    {"type", LogicalType::VARCHAR},
	    {"subtype", LogicalType::VARCHAR},
	    {"nullable", LogicalType::BOOLEAN},
	    {"unique", LogicalType::BOOLEAN},
	    {"width", LogicalType::BIGINT},
	    {"precision", LogicalType::BIGINT},
	});
}

LogicalType GetLayerType() {
	return LogicalType::STRUCT({
	    {"name", LogicalType::VARCHAR},
	    {"feature_count", LogicalType::BIGINT},
	    {"geometry_fields", LogicalType::LIST(GetGeometryFieldType())},
	    {"fields", LogicalType::LIST(GetStandardFieldType())},
	});
}

auto Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &types, vector<Identifier> &names)
    -> unique_ptr<FunctionData> {
	names.push_back("file_name");
	names.push_back("driver_short_name");
	names.push_back("driver_long_name");
	names.push_back("layers");

	types.push_back(LogicalType::VARCHAR);
	types.push_back(LogicalType::VARCHAR);
	types.push_back(LogicalType::VARCHAR);
	types.push_back(LogicalType::LIST(GetLayerType()));

	auto result = make_uniq<BindData>();

	// For /vsi* paths and GDAL driver-prefixed URLs (e.g., "WFS:https://..."),
	// bypass MultiFileReader since these are not local file globs.
	// We must check the input type first because MultiFileReader::CreateFunctionSet
	// adds a VARCHAR[] overload, and GetValue<string>() on a LIST would return
	// the string representation of the list, not an actual path.
	auto &input_val = input.inputs[0];
	if (input_val.type().id() == LogicalTypeId::VARCHAR) {
		auto raw_path = input_val.GetValue<string>();
		if (StringUtil::StartsWith(raw_path, "/vsi") || DuckDBFileSystemPrefix::IsGDALDriverPrefixedURL(raw_path)) {
			result->files.emplace_back(std::move(raw_path));
		} else {
			const auto mf_reader = MultiFileReader::Create(input.table_function);
			const auto mf_inputs = mf_reader->CreateFileList(context, input_val, FileGlobOptions::ALLOW_EMPTY);
			result->files = mf_inputs->GetAllFiles();
		}
	} else {
		const auto mf_reader = MultiFileReader::Create(input.table_function);
		const auto mf_inputs = mf_reader->CreateFileList(context, input_val, FileGlobOptions::ALLOW_EMPTY);
		result->files = mf_inputs->GetAllFiles();
	}

	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Global State
//----------------------------------------------------------------------------------------------------------------------
class GlobalState final : public GlobalTableFunctionState {
public:
	idx_t current_idx = 0;
};

auto InitGlobal(ClientContext &context, TableFunctionInitInput &input) -> unique_ptr<GlobalTableFunctionState> {
	auto result = make_uniq<GlobalState>();
	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Scan
//----------------------------------------------------------------------------------------------------------------------
static Value GetLayerData(const GDALDatasetUniquePtr &dataset) {

	vector<Value> layer_values;
	for (const auto &layer : dataset->GetLayers()) {
		child_list_t<Value> layer_value_fields;

		layer_value_fields.emplace_back("name", Value(layer->GetName()));
		layer_value_fields.emplace_back("feature_count", Value(static_cast<int64_t>(layer->GetFeatureCount())));

		vector<Value> geometry_fields;
		for (const auto &field : layer->GetLayerDefn()->GetGeomFields()) {
			child_list_t<Value> geometry_field_value_fields;
			auto field_name = field->GetNameRef();
			if (std::strlen(field_name) == 0) {
				field_name = "geom";
			}
			geometry_field_value_fields.emplace_back("name", Value(field_name));
			geometry_field_value_fields.emplace_back("type", Value(OGRGeometryTypeToName(field->GetType())));
			geometry_field_value_fields.emplace_back("nullable", Value(static_cast<bool>(field->IsNullable())));

			const auto crs = field->GetSpatialRef();
			if (crs != nullptr) {
				child_list_t<Value> crs_value_fields;
				crs_value_fields.emplace_back("name", Value(crs->GetName()));
				crs_value_fields.emplace_back("auth_name", Value(crs->GetAuthorityName(nullptr)));
				crs_value_fields.emplace_back("auth_code", Value(crs->GetAuthorityCode(nullptr)));

				char *wkt_ptr = nullptr;
				crs->exportToWkt(&wkt_ptr);
				crs_value_fields.emplace_back("wkt", wkt_ptr ? Value(wkt_ptr) : Value());
				CPLFree(wkt_ptr);

				char *proj4_ptr = nullptr;
				crs->exportToProj4(&proj4_ptr);
				crs_value_fields.emplace_back("proj4", proj4_ptr ? Value(proj4_ptr) : Value());
				CPLFree(proj4_ptr);

				char *projjson_ptr = nullptr;
				crs->exportToPROJJSON(&projjson_ptr, nullptr);
				crs_value_fields.emplace_back("projjson", projjson_ptr ? Value(projjson_ptr) : Value());
				CPLFree(projjson_ptr);

				geometry_field_value_fields.emplace_back("crs", Value::STRUCT(crs_value_fields));
			} else {
				Value null_crs;
				geometry_field_value_fields.emplace_back("crs", null_crs);
			}

			geometry_fields.push_back(Value::STRUCT(geometry_field_value_fields));
		}
		layer_value_fields.emplace_back("geometry_fields",
		                                Value::LIST(GetGeometryFieldType(), std::move(geometry_fields)));

		vector<Value> standard_fields;
		for (const auto &field : layer->GetLayerDefn()->GetFields()) {
			child_list_t<Value> standard_field_value_fields;
			standard_field_value_fields.emplace_back("name", Value(field->GetNameRef()));
			standard_field_value_fields.emplace_back("type", Value(OGR_GetFieldTypeName(field->GetType())));
			standard_field_value_fields.emplace_back("subtype", Value(OGR_GetFieldSubTypeName(field->GetSubType())));
			standard_field_value_fields.emplace_back("nullable", Value(field->IsNullable()));
			standard_field_value_fields.emplace_back("unique", Value(field->IsUnique()));
			standard_field_value_fields.emplace_back("width", Value(field->GetWidth()));
			standard_field_value_fields.emplace_back("precision", Value(field->GetPrecision()));
			standard_fields.push_back(Value::STRUCT(standard_field_value_fields));
		}
		layer_value_fields.emplace_back("fields", Value::LIST(GetStandardFieldType(), std::move(standard_fields)));

		layer_values.push_back(Value::STRUCT(layer_value_fields));
	}

	return Value::LIST(GetLayerType(), std::move(layer_values));
}

void Scan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bdata = input.bind_data->Cast<BindData>();
	auto &gstate = input.global_state->Cast<GlobalState>();

	const auto &file_prefix = DuckDBFileSystemPrefix::GetOrCreate(context);

	const auto remaining = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bdata.files.size() - gstate.current_idx);
	auto output_idx = 0;

	for (idx_t in_idx = 0; in_idx < remaining; in_idx++, gstate.current_idx++) {
		auto &file = bdata.files[gstate.current_idx];
		auto prefixed_file_name = file_prefix.AddPrefix(file.path);

		GDALDatasetUniquePtr dataset;
		try {
			dataset = GDALDatasetUniquePtr(
			    GDALDataset::Open(prefixed_file_name.c_str(), GDAL_OF_VECTOR | GDAL_OF_VERBOSE_ERROR));
		} catch (...) {
			// Just skip anything we cant open
			continue;
		}

		output.data[0].SetValue(output_idx, file.path);
		output.data[1].SetValue(output_idx, dataset->GetDriver()->GetDescription());
		output.data[2].SetValue(output_idx, dataset->GetDriver()->GetMetadataItem(GDAL_DMD_LONGNAME));
		output.data[3].SetValue(output_idx, GetLayerData(dataset));

		output_idx++;
	}

	output.SetChildCardinality(output_idx);
}

//----------------------------------------------------------------------------------------------------------------------
// Register
//----------------------------------------------------------------------------------------------------------------------
static constexpr auto DESCRIPTION = R"(
	    Read the metadata from a variety of geospatial file formats using the GDAL library.

	    The `ST_Read_Meta` table function accompanies the `ST_Read` table function, but instead of reading the contents of a file, this function scans the metadata instead.
	    Since the data model of the underlying GDAL library is quite flexible, most of the interesting metadata is within the returned `layers` column, which is a somewhat complex nested structure of DuckDB `STRUCT` and `LIST` types.
	)";

static constexpr auto EXAMPLE = R"(
	    -- Find the coordinate reference system authority name and code for the first layers first geometry column in the file
	    SELECT
	        layers[1].geometry_fields[1].crs.auth_name as name,
	        layers[1].geometry_fields[1].crs.auth_code as code
	    FROM st_read_meta('../../tmp/data/amsterdam_roads.fgb');
	)";

static void Register(ExtensionLoader &loader) {
	const TableFunction func("ST_Read_Meta", {LogicalType::VARCHAR}, Scan, Bind, InitGlobal);
	loader.RegisterFunction(MultiFileReader::CreateFunctionSet(func));

	InsertionOrderPreservingMap<string> tags;
	tags.insert("ext", "spatial");
	FunctionBuilder::AddTableFunctionDocs(loader, "ST_Read_Meta", DESCRIPTION, EXAMPLE, tags);
}

} // namespace gdal_meta
//======================================================================================================================
// GDAL MODULE
//======================================================================================================================
void RegisterGDALModule(ExtensionLoader &loader) {

	// Load GDAL (once)
	static std::once_flag loaded;
	std::call_once(loaded, [&]() {
		// Register all embedded drivers (dont go looking for plugins)
		OGRRegisterAllInternal();

		// GDAL error handler. For ordinary failures we MUST NOT throw from here: GDAL is not exception-safe and an
		// exception thrown from the handler unwinds through GDAL's own frames, leaking memory and potentially
		// corrupting its state. Those are recorded in GDAL's thread-local state (CPLGetLastError*) and surfaced at our
		// call boundaries via ThrowGDALError().
		//
		// CE_Fatal is the exception: GDAL would otherwise abort() the whole process right after the handler returns.
		// Throwing here pre-empts that abort and lets DuckDB invalidate the database instead of hard-crashing. We
		// accept the risk of unwinding through GDAL on a fatal error since the process was going to die regardless.
		CPLSetErrorHandler([](CPLErr e, int code, const char *raw_msg) {
			if (e == CE_Fatal) {
				throw InternalException("Fatal GDAL error: " + CleanGDALMessage(raw_msg));
			}
			// Otherwise a no-op: the error is captured in GDAL's thread-local state and surfaced at the boundary.
		});
	});

	gdal_read::Register(loader);
	gdal_copy::Register(loader);
	gdal_list::Register(loader);
	gdal_meta::Register(loader);
}
} // namespace duckdb
