#include "spatial/modules/gdal/gdal_module.hpp"

// Spatial
#include "spatial/spatial_types.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/geometry/wkb_writer.hpp"
#include "spatial/geometry/geometry_serialization.hpp"
#include "spatial/util/function_builder.hpp"

// DuckDB
#include "duckdb/main/database.hpp"
#include "duckdb/common/enums/file_glob_options.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

// GDAL
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "cpl_vsi_error.h"
#include "cpl_vsi_virtual.h"
#include "ogrsf_frmts.h"

namespace duckdb {

namespace {

//######################################################################################################################
// DuckDB GDAL VFS
//######################################################################################################################
// This implements a GDAL "VFS" (Virtual File System) that allows GDAL to read and write files from DuckDB's file
// system
// TODO: Make another pass at this, we should be able to clean it up a bit more.

class DuckDBFileHandle final : public VSIVirtualHandle {
private:
	unique_ptr<FileHandle> file_handle;
	bool is_eof;

public:
	explicit DuckDBFileHandle(unique_ptr<FileHandle> file_handle_p)
	    : file_handle(std::move(file_handle_p)), is_eof(false) {
	}

	vsi_l_offset Tell() override {
		return static_cast<vsi_l_offset>(file_handle->SeekPosition());
	}
	int Seek(vsi_l_offset nOffset, int nWhence) override {
		is_eof = false;

		if (nWhence == SEEK_SET && nOffset == 0) {
			// Use the reset function instead to allow compressed file handles to rewind
			// even if they don't support seeking
			file_handle->Reset();
			return 0;
		}
		switch (nWhence) {
		case SEEK_SET:
			file_handle->Seek(nOffset);
			break;
		case SEEK_CUR:
			file_handle->Seek(file_handle->SeekPosition() + nOffset);
			break;
		case SEEK_END:
			file_handle->Seek(file_handle->GetFileSize() + nOffset);
			break;
		default:
			throw InternalException("Unknown seek type");
		}
		return 0;
	}

	size_t Read(void *pBuffer, size_t nSize, size_t nCount) override {
		auto remaining_bytes = nSize * nCount;
		try {
			while (remaining_bytes > 0) {
				auto read_bytes = file_handle->Read(pBuffer, remaining_bytes);
				if (read_bytes == 0) {
					break;
				}
				remaining_bytes -= read_bytes;
				// Note we performed a cast back to void*
				pBuffer = static_cast<uint8_t *>(pBuffer) + read_bytes;
			}
		} catch (...) {
		}

		if (remaining_bytes != 0) {
			if (file_handle->SeekPosition() == file_handle->GetFileSize()) {
				// Is at EOF!
				is_eof = true;
			}
			// else, error!
			// unfortunately, this version of GDAL cant distinguish between errors and reading less bytes
			// its avaiable in 3.9.2, but we're stuck on 3.8.5 for now.
		}

		return nCount - (remaining_bytes / nSize);
	}

	int Eof() override {
		return is_eof ? TRUE : FALSE;
	}

	size_t Write(const void *pBuffer, size_t nSize, size_t nCount) override {
		size_t written_bytes = 0;
		try {
			written_bytes = file_handle->Write(const_cast<void *>(pBuffer), nSize * nCount);
		} catch (...) {
		}
		// Return the number of items written
		return static_cast<size_t>(written_bytes / nSize);
	}

	int Flush() override {
		file_handle->Sync();
		return 0;
	}
	int Truncate(vsi_l_offset nNewSize) override {
		file_handle->Truncate(static_cast<int64_t>(nNewSize));
		return 0;
	}
	int Close() override {
		file_handle->Close();
		return 0;
	}

	// int ReadMultiRange(int nRanges, void **ppData, const vsi_l_offset *panOffsets, const size_t *panSizes) override;
	// void AdviseRead(int nRanges, const vsi_l_offset *panOffsets, const size_t *panSizes) override;
	// VSIRangeStatus GetRangeStatus(vsi_l_offset nOffset, vsi_l_offset nLength) override;
};

//--------------------------------------------------------------------------
// GDAL DuckDB File system wrapper
//--------------------------------------------------------------------------
bool IsStdCharDev(const char *file_name) {
	return !strcmp(file_name, "/dev/stdin") || !strcmp(file_name, "/dev/stdout") || !strcmp(file_name, "/dev/stderr") ||
	       !strcmp(file_name, "/dev/null") || !strcmp(file_name, "/dev/zero");
}

class DuckDBFileSystemHandler final : public VSIFilesystemHandler {
private:
	string client_prefix;
	ClientContext &context;

public:
	DuckDBFileSystemHandler(string client_prefix, ClientContext &context)
	    : client_prefix(std::move(client_prefix)), context(context) {};

	const char *StripPrefix(const char *pszFilename) {
		return pszFilename + client_prefix.size();
	}

	string AddPrefix(const string &value) {
		return client_prefix + value;
	}

	VSIVirtualHandle *Open(const char *prefixed_file_name, const char *access, bool bSetError,
	                       CSLConstList /* papszOptions */) override {
		auto file_name = StripPrefix(prefixed_file_name);
		auto file_name_str = string(file_name);
		auto &fs = FileSystem::GetFileSystem(context);

		// TODO: Double check that this is correct
		FileOpenFlags flags;
		auto len = strlen(access);
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
			if (!IsStdCharDev(file_name)) {
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
			throw InternalException("Unknown file access type");
		}

		try {
			// Check if the file is a directory

#ifdef _WIN32
			if (!FileSystem::IsRemoteFile(file_name) && fs.DirectoryExists(file_name_str) && (flags.OpenForReading())) {
				// We can't open a directory for reading on windows without special flags
				// so just open nul instead, gdal will reject it when it tries to read
				auto file = fs.OpenFile("nul", flags);
				return new DuckDBFileHandle(std::move(file));
			}
#endif

			// If the file is remote and NOT in write mode, we can cache it.
			if (FileSystem::IsRemoteFile(file_name_str) && !flags.OpenForWriting() && !flags.OpenForAppending()) {

				// Pass the direct IO flag to the file system since we use GDAL's caching instead
				flags |= FileFlags::FILE_FLAGS_DIRECT_IO;

				auto file = fs.OpenFile(file_name, flags | FileCompressionType::AUTO_DETECT);
				return VSICreateCachedFile(new DuckDBFileHandle(std::move(file)));
			} else {
				auto file = fs.OpenFile(file_name, flags | FileCompressionType::AUTO_DETECT);
				return new DuckDBFileHandle(std::move(file));
			}
		} catch (std::exception &ex) {
			// Failed to open file via DuckDB File System. If this doesn't have a VSI prefix we can return an error here.
			if (strncmp(file_name, "/vsi", 4) != 0 && !IsStdCharDev(file_name)) {
				if (bSetError) {
					VSIError(VSIE_FileError, "Failed to open file %s: %s", file_name, ex.what());
				}
				return nullptr;
			}

			// Fall back to GDAL instead (if external access is enabled)
			if (!context.db->config.options.enable_external_access) {
				if (bSetError) {
					VSIError(VSIE_FileError, "Failed to open file %s with GDAL: External access is disabled",
					         file_name);
				}
				return nullptr;
			}

			const auto handler = VSIFileManager::GetHandler(file_name);
			if (!handler) {
				if (bSetError) {
					VSIError(VSIE_FileError, "Failed to open file %s: %s", file_name, ex.what());
				}
				return nullptr;
			}

			return handler->Open(file_name, access);
		}
	}

	int Stat(const char *prefixed_file_name, VSIStatBufL *pstatbuf, int n_flags) override {
		auto file_name = StripPrefix(prefixed_file_name);
		auto &fs = FileSystem::GetFileSystem(context);

		memset(pstatbuf, 0, sizeof(VSIStatBufL));

		if (IsStdCharDev(file_name)) {
			pstatbuf->st_mode = S_IFCHR;
			return 0;
		}

		if (!(fs.FileExists(file_name) || (!FileSystem::IsRemoteFile(file_name) && fs.DirectoryExists(file_name)))) {
			return -1;
		}

#ifdef _WIN32
		if (!FileSystem::IsRemoteFile(file_name) && fs.DirectoryExists(file_name)) {
			pstatbuf->st_mode = S_IFDIR;
			return 0;
		}
#endif

		unique_ptr<FileHandle> file;
		try {
			file = fs.OpenFile(file_name, FileFlags::FILE_FLAGS_READ | FileCompressionType::AUTO_DETECT |
			                                  FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
		} catch (std::exception &ex) {
			return -1;
		}
		if (!file) {
			return -1;
		}

		pstatbuf->st_size = static_cast<off_t>(fs.GetFileSize(*file));
		pstatbuf->st_mtime = Timestamp::ToTimeT(fs.GetLastModifiedTime(*file));

		auto type = file->GetType();
		switch (type) {
		// These are the only three types present on all platforms
		case FileType::FILE_TYPE_REGULAR:
			pstatbuf->st_mode = S_IFREG;
			break;
		case FileType::FILE_TYPE_DIR:
			pstatbuf->st_mode = S_IFDIR;
			break;
		case FileType::FILE_TYPE_CHARDEV:
			pstatbuf->st_mode = S_IFCHR;
			break;
		default:
			// HTTPFS returns invalid type for everything basically.
			if (FileSystem::IsRemoteFile(file_name)) {
				pstatbuf->st_mode = S_IFREG;
			} else {
				return -1;
			}
		}

		return 0;
	}

	bool IsLocal(const char *prefixed_file_name) override {
		auto file_name = StripPrefix(prefixed_file_name);
		return !FileSystem::IsRemoteFile(file_name);
	}

	int Mkdir(const char *prefixed_dir_name, long mode) override {
		auto dir_name = StripPrefix(prefixed_dir_name);
		auto &fs = FileSystem::GetFileSystem(context);

		fs.CreateDirectory(dir_name);
		return 0;
	}

	int Rmdir(const char *prefixed_dir_name) override {
		auto dir_name = StripPrefix(prefixed_dir_name);
		auto &fs = FileSystem::GetFileSystem(context);

		fs.RemoveDirectory(dir_name);
		return 0;
	}

	int RmdirRecursive(const char *prefixed_dir_name) override {
		auto dir_name = StripPrefix(prefixed_dir_name);
		auto &fs = FileSystem::GetFileSystem(context);

		fs.RemoveDirectory(dir_name);
		return 0;
	}

	char **ReadDirEx(const char *prefixed_dir_name, int max_files) override {
		auto dir_name = StripPrefix(prefixed_dir_name);
		auto &fs = FileSystem::GetFileSystem(context);

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
	}

	char **SiblingFiles(const char *prefixed_file_name) override {
		auto file_name = StripPrefix(prefixed_file_name);

		auto &fs = FileSystem::GetFileSystem(context);
		CPLStringList files;

		auto file_name_without_ext =
		    fs.JoinPath(StringUtil::GetFilePath(file_name), StringUtil::GetFileStem(file_name));
		auto file_glob = file_name_without_ext + ".*";

		auto file_vector = fs.Glob(file_glob);
		for (auto &file : file_vector) {
			auto tmp = AddPrefix(file.path);
			files.AddString(tmp.c_str());
		}
		return files.StealList();
	}

	int HasOptimizedReadMultiRange(const char *pszPath) override {
		return 0;
	}

	int Unlink(const char *prefixed_file_name) override {
		auto file_name = StripPrefix(prefixed_file_name);
		auto &fs = FileSystem::GetFileSystem(context);
		try {
			fs.RemoveFile(file_name);
			return 0;
		} catch (std::exception &ex) {
			return -1;
		}
	}
};

//######################################################################################################################
// Context State
//######################################################################################################################
// We give every client a unique prefix so that multiple connections can use their own attached file systems.
// This is necessary because GDAL is not otherwise aware of the connection context.

class GDALClientContextState final : public ClientContextState {
	ClientContext &context;
	string client_prefix;
	DuckDBFileSystemHandler *fs_handler;

public:
	explicit GDALClientContextState(ClientContext &context);
	~GDALClientContextState() override;
	void QueryEnd() override;
	string GetPrefix(const string &value) const;
	static GDALClientContextState &GetOrCreate(ClientContext &context);
};

GDALClientContextState::GDALClientContextState(ClientContext &context) : context(context) {

	// Create a new random prefix for this client
	client_prefix = StringUtil::Format("/vsiduckdb-%s/", UUID::ToString(UUID::GenerateRandomUUID()));

	// Create a new file handler responding to this prefix
	fs_handler = new DuckDBFileSystemHandler(client_prefix, context);

	// Register the file handler
	VSIFileManager::InstallHandler(client_prefix, fs_handler);

	// Also pass a reference to the client context
}

GDALClientContextState::~GDALClientContextState() {
	// Uninstall the file handler for this prefix
	VSIFileManager::RemoveHandler(client_prefix);

	// Delete the file handler
	delete fs_handler;
}

void GDALClientContextState::QueryEnd() {
}

string GDALClientContextState::GetPrefix(const string &value) const {
	// If the user explicitly asked for a VSI prefix, we don't add our own
	if (StringUtil::StartsWith(value, "/vsi")) {
		if (!context.db->config.options.enable_external_access) {
			throw PermissionException("Cannot open file '%s' with VSI prefix: External access is disabled", value);
		}
		return value;
	}
	return client_prefix + value;
}

GDALClientContextState &GDALClientContextState::GetOrCreate(ClientContext &context) {
	auto gdal_state = context.registered_state->GetOrCreate<GDALClientContextState>("gdal", context);
	return *gdal_state;
}

//######################################################################################################################
// Functions
//######################################################################################################################

//======================================================================================================================
// ST_Read
//======================================================================================================================

struct ST_Read : ArrowTableFunction {

	//------------------------------------------------------------------------------------------------------------------
	// Misc
	//------------------------------------------------------------------------------------------------------------------
	enum class SpatialFilterType { Wkb, Rectangle };

	struct SpatialFilter {
		SpatialFilterType type;
		explicit SpatialFilter(SpatialFilterType type_p) : type(type_p) {};
	};

	struct RectangleSpatialFilter : SpatialFilter {
		double min_x, min_y, max_x, max_y;
		RectangleSpatialFilter(double min_x_p, double min_y_p, double max_x_p, double max_y_p)
		    : SpatialFilter(SpatialFilterType::Rectangle), min_x(min_x_p), min_y(min_y_p), max_x(max_x_p),
		      max_y(max_y_p) {
		}
	};

	struct WKBSpatialFilter : SpatialFilter {
		OGRGeometryH geom;
		explicit WKBSpatialFilter(const string &wkb_p) : SpatialFilter(SpatialFilterType::Wkb), geom(nullptr) {
			auto ok = OGR_G_CreateFromWkb(wkb_p.c_str(), nullptr, &geom, (int)wkb_p.size());
			if (ok != OGRERR_NONE) {
				throw InvalidInputException("WKBSpatialFilter: could not create geometry from WKB");
			}
		}
		~WKBSpatialFilter() {
			OGR_G_DestroyGeometry(geom);
		}
	};

	static void TryApplySpatialFilter(OGRLayer *layer, SpatialFilter *spatial_filter) {
		if (spatial_filter != nullptr) {
			if (spatial_filter->type == SpatialFilterType::Rectangle) {
				auto &rect = static_cast<RectangleSpatialFilter &>(*spatial_filter);
				layer->SetSpatialFilterRect(rect.min_x, rect.min_y, rect.max_x, rect.max_y);
			} else if (spatial_filter->type == SpatialFilterType::Wkb) {
				auto &filter = static_cast<WKBSpatialFilter &>(*spatial_filter);
				layer->SetSpatialFilter(OGRGeometry::FromHandle(filter.geom));
			}
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	struct BindData final : TableFunctionData {

		int layer_idx = 0;
		bool sequential_layer_scan = false;
		bool keep_wkb = false;
		unordered_set<idx_t> geometry_column_ids = {};
		unique_ptr<SpatialFilter> spatial_filter = nullptr;

		// before they are renamed
		vector<string> all_names = {};
		vector<LogicalType> all_types = {};
		ArrowTableSchema arrow_table = {};

		bool has_approximate_feature_count = false;
		idx_t approximate_feature_count = 0;
		string raw_file_name;
		string prefixed_file_name;
		CPLStringList dataset_open_options;
		CPLStringList dataset_allowed_drivers;
		CPLStringList dataset_sibling_files;
		CPLStringList layer_creation_options;
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<string> &names) {

		// Result
		auto result = make_uniq<BindData>();

		auto options_param = input.named_parameters.find("open_options");
		if (options_param != input.named_parameters.end()) {
			for (auto &param : ListValue::GetChildren(options_param->second)) {
				result->dataset_open_options.AddString(StringValue::Get(param).c_str());
			}
		}

		auto drivers_param = input.named_parameters.find("allowed_drivers");
		if (drivers_param != input.named_parameters.end()) {
			for (auto &param : ListValue::GetChildren(drivers_param->second)) {
				result->dataset_allowed_drivers.AddString(StringValue::Get(param).c_str());
			}
		}

		// Now we can open the dataset
		auto &ctx_state = GDALClientContextState::GetOrCreate(context);

		auto siblings_params = input.named_parameters.find("sibling_files");
		if (siblings_params != input.named_parameters.end()) {
			for (auto &param : ListValue::GetChildren(siblings_params->second)) {
				result->dataset_sibling_files.AddString(ctx_state.GetPrefix(StringValue::Get(param)).c_str());
			}
		}

		result->raw_file_name = input.inputs[0].GetValue<string>();
		result->prefixed_file_name = ctx_state.GetPrefix(result->raw_file_name);

		auto dataset = GDALDatasetUniquePtr(GDALDataset::Open(
		    result->prefixed_file_name.c_str(), GDAL_OF_VECTOR | GDAL_OF_VERBOSE_ERROR, result->dataset_allowed_drivers,
		    result->dataset_open_options, result->dataset_sibling_files));

		if (dataset == nullptr) {
			auto error = string(CPLGetLastErrorMsg());
			throw IOException("Could not open file: " + result->raw_file_name + " (" + error + ")");
		}

		// Double check that the dataset have any layers
		if (dataset->GetLayerCount() <= 0) {
			throw IOException("Dataset does not contain any layers");
		}

		// Now we can bind the additonal options
		bool max_batch_size_set = false;
		for (auto &kv : input.named_parameters) {
			auto loption = StringUtil::Lower(kv.first);
			if (loption == "layer") {

				// Find layer by index
				if (kv.second.type() == LogicalType::INTEGER) {
					auto layer_idx = IntegerValue::Get(kv.second);
					if (layer_idx < 0) {
						throw BinderException("Layer index must be positive");
					}
					if (layer_idx > dataset->GetLayerCount()) {
						throw BinderException(
						    StringUtil::Format("Layer index too large (%s > %s)", layer_idx, dataset->GetLayerCount()));
					}
					result->layer_idx = layer_idx;
				}

				// Find layer by name
				if (kv.second.type() == LogicalTypeId::VARCHAR) {
					auto name = StringValue::Get(kv.second).c_str();
					bool found = false;
					for (auto layer_idx = 0; layer_idx < dataset->GetLayerCount(); layer_idx++) {
						if (strcmp(dataset->GetLayer(layer_idx)->GetName(), name) == 0) {
							result->layer_idx = layer_idx;
							found = true;
							break;
						}
					}
					if (!found) {
						throw BinderException(StringUtil::Format("Layer '%s' could not be found in dataset", name));
					}
				}
			}

			if (loption == "spatial_filter_box" && kv.second.type() == GeoTypes::BOX_2D()) {
				if (result->spatial_filter) {
					throw BinderException("Only one spatial filter can be specified");
				}
				auto &children = StructValue::GetChildren(kv.second);
				auto minx = DoubleValue::Get(children[0]);
				auto miny = DoubleValue::Get(children[1]);
				auto maxx = DoubleValue::Get(children[2]);
				auto maxy = DoubleValue::Get(children[3]);
				result->spatial_filter = make_uniq<RectangleSpatialFilter>(minx, miny, maxx, maxy);
			}

			if (loption == "spatial_filter" && kv.second.type() == GeoTypes::WKB_BLOB()) {
				if (result->spatial_filter) {
					throw BinderException("Only one spatial filter can be specified");
				}
				auto wkb = StringValue::Get(kv.second);
				result->spatial_filter = make_uniq<WKBSpatialFilter>(wkb);
			}

			if (loption == "sequential_layer_scan") {
				result->sequential_layer_scan = BooleanValue::Get(kv.second);
			}

			if (loption == "max_batch_size") {
				auto max_batch_size = IntegerValue::Get(kv.second);
				if (max_batch_size <= 0) {
					throw BinderException("'max_batch_size' parameter must be positive");
				}
				auto str = StringUtil::Format("MAX_FEATURES_IN_BATCH=%d", max_batch_size);
				result->layer_creation_options.AddString(str.c_str());
				max_batch_size_set = true;
			}

			if (loption == "keep_wkb") {
				result->keep_wkb = BooleanValue::Get(kv.second);
			}
		}

		// Defaults
		result->layer_creation_options.AddString("INCLUDE_FID=NO");
		if (!max_batch_size_set) {
			// Set default max batch size to standard vector size
			auto str = StringUtil::Format("MAX_FEATURES_IN_BATCH=%d", STANDARD_VECTOR_SIZE);
			result->layer_creation_options.AddString(str.c_str());
		}

		// Get the schema for the selected layer
		auto layer = dataset->GetLayer(result->layer_idx);

		TryApplySpatialFilter(layer, result->spatial_filter.get());

		// Check if we can get an approximate feature count
		result->approximate_feature_count = 0;
		result->has_approximate_feature_count = false;
		if (!result->sequential_layer_scan) {
			// Dont force compute the count if its expensive
			auto count = layer->GetFeatureCount(false);
			if (count > -1) {
				result->approximate_feature_count = count;
				result->has_approximate_feature_count = true;
			}
		}

		struct ArrowArrayStream stream;
		if (!layer->GetArrowStream(&stream, result->layer_creation_options)) {
			// layer is owned by GDAL, we do not need to destory it
			throw IOException("Could not get arrow stream from layer");
		}

		struct ArrowSchema schema;
		if (stream.get_schema(&stream, &schema) != 0) {
			if (stream.release) {
				stream.release(&stream);
			}
			throw IOException("Could not get arrow schema from layer");
		}

		// The Arrow API will return attributes in this order
		// 1. FID column
		// 2. all ogr field attributes
		// 3. all geometry columns

		auto attribute_count = schema.n_children;
		auto attributes = schema.children;

		result->all_names.reserve(attribute_count + 1);
		names.reserve(attribute_count + 1);

		for (idx_t col_idx = 0; col_idx < (idx_t)attribute_count; col_idx++) {
			auto &attribute = *attributes[col_idx];

			const char ogc_flag[] = {'\x01', '\0', '\0', '\0', '\x14', '\0', '\0', '\0', 'A', 'R', 'R', 'O', 'W',
			                         ':',    'e',  'x',  't',  'e',    'n',  's',  'i',  'o', 'n', ':', 'n', 'a',
			                         'm',    'e',  '\a', '\0', '\0',   '\0', 'o',  'g',  'c', '.', 'w', 'k', 'b'};

			auto arrow_type = ArrowType::GetArrowLogicalType(DBConfig::GetConfig(context), attribute);

			auto column_name = string(attribute.name);
			auto duckdb_type = arrow_type->GetDuckType();

			if (duckdb_type.id() == LogicalTypeId::BLOB && attribute.metadata != nullptr &&
			    strncmp(attribute.metadata, ogc_flag, sizeof(ogc_flag)) == 0) {
				// This is a WKB geometry blob
				result->arrow_table.AddColumn(col_idx, std::move(arrow_type), column_name);

				if (result->keep_wkb) {
					return_types.emplace_back(GeoTypes::WKB_BLOB());
				} else {
					return_types.emplace_back(GeoTypes::GEOMETRY());
					if (column_name == "wkb_geometry") {
						column_name = "geom";
					}
				}
				result->geometry_column_ids.insert(col_idx);

			} else if (attribute.dictionary) {
				auto dictionary_type = ArrowType::GetArrowLogicalType(DBConfig::GetConfig(context), attribute);
				return_types.emplace_back(dictionary_type->GetDuckType());
				arrow_type->SetDictionary(std::move(dictionary_type));
				result->arrow_table.AddColumn(col_idx, std::move(arrow_type), column_name);
			} else {
				return_types.emplace_back(arrow_type->GetDuckType());
				result->arrow_table.AddColumn(col_idx, std::move(arrow_type), column_name);
			}

			// keep these around for projection/filter pushdown later
			// does GDAL even allow duplicate/missing names?
			result->all_names.push_back(column_name);

			if (column_name.empty()) {
				names.push_back("v" + to_string(col_idx));
			} else {
				names.push_back(column_name);
			}
		}

		result->all_types = return_types;

		schema.release(&schema);
		stream.release(&stream);

		// Rename columns if they are duplicates
		unordered_map<string, idx_t> name_map;
		for (auto &column_name : names) {
			// put it all lower_case
			auto low_column_name = StringUtil::Lower(column_name);
			if (name_map.find(low_column_name) == name_map.end()) {
				// Name does not exist yet
				name_map[low_column_name]++;
			} else {
				// Name already exists, we add _x where x is the repetition number
				string new_column_name = column_name + "_" + std::to_string(name_map[low_column_name]);
				auto new_column_name_low = StringUtil::Lower(new_column_name);
				while (name_map.find(new_column_name_low) != name_map.end()) {
					// This name is already here due to a previous definition
					name_map[low_column_name]++;
					new_column_name = column_name + "_" + std::to_string(name_map[low_column_name]);
					new_column_name_low = StringUtil::Lower(new_column_name);
				}
				column_name = new_column_name;
				name_map[new_column_name_low]++;
			}
		}

		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init Global
	//------------------------------------------------------------------------------------------------------------------
	struct GlobalState final : ArrowScanGlobalState {
		GDALDatasetUniquePtr dataset;
		atomic<idx_t> lines_read;

		explicit GlobalState(GDALDatasetUniquePtr dataset) : dataset(std::move(dataset)), lines_read(0) {
		}
	};

	static unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input) {
		auto &data = input.bind_data->Cast<BindData>();

		auto dataset = GDALDatasetUniquePtr(GDALDataset::Open(
		    data.prefixed_file_name.c_str(), GDAL_OF_VECTOR | GDAL_OF_VERBOSE_ERROR | GDAL_OF_READONLY,
		    data.dataset_allowed_drivers, data.dataset_open_options, data.dataset_sibling_files));
		if (dataset == nullptr) {
			const auto error = string(CPLGetLastErrorMsg());
			throw IOException("Could not open file: " + data.raw_file_name + " (" + error + ")");
		}

		auto global_state = make_uniq<GlobalState>(std::move(dataset));
		auto &gstate = *global_state;

		// Open the layer
		OGRLayer *layer = nullptr;
		if (data.sequential_layer_scan) {
			// Get the layer from the dataset by scanning through the layers
			for (int i = 0; i < gstate.dataset->GetLayerCount(); i++) {
				layer = gstate.dataset->GetLayer(i);
				if (i == data.layer_idx) {
					// desired layer found
					break;
				}
				// else scan through and empty the layer
				OGRFeature *feature;
				while ((feature = layer->GetNextFeature()) != nullptr) {
					OGRFeature::DestroyFeature(feature);
				}
			}
		} else {
			// Otherwise get the layer directly
			layer = gstate.dataset->GetLayer(data.layer_idx);
		}
		if (!layer) {
			throw IOException("Could not get layer");
		}

		// Apply spatial filter (if we got one)
		TryApplySpatialFilter(layer, data.spatial_filter.get());
		// TODO: Apply projection pushdown

		// Create arrow stream from layer

		gstate.stream = make_uniq<ArrowArrayStreamWrapper>();

		// set layer options
		if (!layer->GetArrowStream(&gstate.stream->arrow_array_stream, data.layer_creation_options)) {
			throw IOException("Could not get arrow stream");
		}

		// Set max 1 thread
		gstate.max_threads = 1;

		if (input.CanRemoveFilterColumns()) {
			gstate.projection_ids = input.projection_ids;
			for (const auto &col_idx : input.column_ids) {
				if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
					gstate.scanned_types.emplace_back(LogicalType::ROW_TYPE);
				} else {
					gstate.scanned_types.push_back(data.all_types[col_idx]);
				}
			}
		}

		return std::move(global_state);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init Local
	//------------------------------------------------------------------------------------------------------------------
	struct LocalState final : ArrowScanLocalState {
		ArenaAllocator arena;
		GeometryAllocator alloc;

		sgl::wkb_reader wkb_reader;

		explicit LocalState(unique_ptr<ArrowArrayWrapper> current_chunk, ClientContext &context)
		    : ArrowScanLocalState(std::move(current_chunk), context), arena(BufferAllocator::Get(context)),
		      alloc(arena), wkb_reader(alloc) {

			// Setup WKB reader
			wkb_reader.set_allow_mixed_zm(true);
			wkb_reader.set_nan_as_empty(true);
		}

		void ConvertWKB(Vector &source, Vector &target, idx_t count) {

			// Reset allocator
			arena.Reset();

			UnaryExecutor::Execute<string_t, string_t>(source, target, count, [&](const string_t &wkb) {
				const auto wkb_ptr = wkb.GetDataUnsafe();
				const auto wkb_len = wkb.GetSize();

				sgl::geometry geom;
				;

				if (!wkb_reader.try_parse(geom, wkb_ptr, wkb_len)) {
					const auto error = wkb_reader.get_error_message();
					throw InvalidInputException("Could not parse WKB input: %s", error);
				}

				// Enforce that we have a cohesive ZM layout
				if (wkb_reader.parsed_mixed_zm()) {
					sgl::ops::force_zm(alloc, geom, wkb_reader.parsed_any_z(), wkb_reader.parsed_any_m(), 0, 0);
				}

				// Serialize the geometry into a blob
				const auto size = Serde::GetRequiredSize(geom);
				auto blob = StringVector::EmptyString(target, size);
				Serde::Serialize(geom, blob.GetDataWriteable(), size);
				blob.Finalize();
				return blob;
			});
		}
	};

	static unique_ptr<LocalTableFunctionState> InitLocal(ExecutionContext &context, TableFunctionInitInput &input,
	                                                     GlobalTableFunctionState *gstate_p) {

		auto &gstate = gstate_p->Cast<ArrowScanGlobalState>();
		auto current_chunk = make_uniq<ArrowArrayWrapper>();
		auto result = make_uniq<LocalState>(std::move(current_chunk), context.client);

		result->column_ids = input.column_ids;
		result->filters = input.filters.get();

		if (input.CanRemoveFilterColumns()) {
			result->all_columns.Initialize(context.client, gstate.scanned_types);
		}

		if (!ArrowTableFunction::ArrowScanParallelStateNext(context.client, input.bind_data.get(), *result, gstate)) {
			return nullptr;
		}

		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		if (!input.local_state) {
			return;
		}

		auto &data = input.bind_data->Cast<BindData>();
		auto &state = input.local_state->Cast<LocalState>();
		auto &gstate = input.global_state->Cast<GlobalState>();

		//! Out of tuples in this chunk
		if (state.chunk_offset >= static_cast<idx_t>(state.chunk->arrow_array.length)) {
			if (!ArrowTableFunction::ArrowScanParallelStateNext(context, input.bind_data.get(), state, gstate)) {
				return;
			}
		}

		auto output_size = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.chunk->arrow_array.length - state.chunk_offset);
		gstate.lines_read += output_size;

		if (gstate.CanRemoveFilterColumns()) {
			state.all_columns.Reset();
			state.all_columns.SetCardinality(output_size);
			ArrowTableFunction::ArrowToDuckDB(state, data.arrow_table.GetColumns(), state.all_columns,
			                                  gstate.lines_read - output_size, false);
			output.ReferenceColumns(state.all_columns, gstate.projection_ids);
		} else {
			output.SetCardinality(output_size);
			ArrowTableFunction::ArrowToDuckDB(state, data.arrow_table.GetColumns(), output,
			                                  gstate.lines_read - output_size, false);
		}

		if (!data.keep_wkb) {
			// Find the geometry columns
			for (idx_t col_idx = 0; col_idx < state.column_ids.size(); col_idx++) {
				auto mapped_idx = state.column_ids[col_idx];
				if (data.geometry_column_ids.find(mapped_idx) != data.geometry_column_ids.end()) {
					// Found a geometry column
					// Convert the WKB columns to a geometry column

					Vector geom_vec(GeoTypes::GEOMETRY(), output_size);
					state.ConvertWKB(output.data[col_idx], geom_vec, output_size);

					output.data[col_idx].ReferenceAndSetType(geom_vec);
				}
			}
		}

		output.Verify();
		state.chunk_offset += output.size();
	}

	//------------------------------------------------------------------------------------------------------------------
	// Cardinality
	//------------------------------------------------------------------------------------------------------------------
	static unique_ptr<NodeStatistics> Cardinality(ClientContext &context, const FunctionData *data) {
		auto &bind_data = data->Cast<BindData>();
		auto result = make_uniq<NodeStatistics>();

		if (bind_data.has_approximate_feature_count) {
			result->has_estimated_cardinality = true;
			result->estimated_cardinality = bind_data.approximate_feature_count;
		}
		return result;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Replacement Scan
	//------------------------------------------------------------------------------------------------------------------
	static unique_ptr<TableRef> ReplacementScan(ClientContext &, ReplacementScanInput &input,
	                                            optional_ptr<ReplacementScanData>) {
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
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
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

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		TableFunction func("ST_Read", {LogicalType::VARCHAR}, Execute, Bind, InitGlobal, InitLocal);

		func.cardinality = Cardinality;
		func.get_partition_data = ArrowTableFunction::ArrowGetPartitionData;

		func.projection_pushdown = true;

		func.named_parameters["open_options"] = LogicalType::LIST(LogicalType::VARCHAR);
		func.named_parameters["allowed_drivers"] = LogicalType::LIST(LogicalType::VARCHAR);
		func.named_parameters["sibling_files"] = LogicalType::LIST(LogicalType::VARCHAR);
		func.named_parameters["spatial_filter_box"] = GeoTypes::BOX_2D();
		func.named_parameters["spatial_filter"] = GeoTypes::WKB_BLOB();
		func.named_parameters["layer"] = LogicalType::VARCHAR;
		func.named_parameters["sequential_layer_scan"] = LogicalType::BOOLEAN;
		func.named_parameters["max_batch_size"] = LogicalType::INTEGER;
		func.named_parameters["keep_wkb"] = LogicalType::BOOLEAN;
		loader.RegisterFunction(func);

		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "spatial");
		FunctionBuilder::AddTableFunctionDocs(loader, "ST_Read", DOCUMENTATION, EXAMPLE, tags);

		// Replacement scan
		auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
		config.replacement_scans.emplace_back(ReplacementScan);
	}
};

//======================================================================================================================
// ST_Read_Meta
//======================================================================================================================
const auto GEOMETRY_FIELD_TYPE = LogicalType::STRUCT({
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

const auto STANDARD_FIELD_TYPE = LogicalType::STRUCT({
    {"name", LogicalType::VARCHAR},
    {"type", LogicalType::VARCHAR},
    {"subtype", LogicalType::VARCHAR},
    {"nullable", LogicalType::BOOLEAN},
    {"unique", LogicalType::BOOLEAN},
    {"width", LogicalType::BIGINT},
    {"precision", LogicalType::BIGINT},
});

const auto LAYER_TYPE = LogicalType::STRUCT({
    {"name", LogicalType::VARCHAR},
    {"feature_count", LogicalType::BIGINT},
    {"geometry_fields", LogicalType::LIST(GEOMETRY_FIELD_TYPE)},
    {"fields", LogicalType::LIST(STANDARD_FIELD_TYPE)},
});

struct ST_Read_Meta {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	struct BindData final : TableFunctionData {
		vector<OpenFileInfo> file_names;

		explicit BindData(vector<OpenFileInfo> file_names_p) : file_names(std::move(file_names_p)) {
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<string> &names) {

		names.push_back("file_name");
		return_types.push_back(LogicalType::VARCHAR);

		names.push_back("driver_short_name");
		return_types.push_back(LogicalType::VARCHAR);

		names.push_back("driver_long_name");
		return_types.push_back(LogicalType::VARCHAR);

		names.push_back("layers");
		return_types.push_back(LogicalType::LIST(LAYER_TYPE));

		// TODO: Add metadata, domains, relationships

		// Get the filename list
		const auto file_name = input.inputs[0].GetValue<string>();

		// If the path starts with /vsi (GDAL virtual filesystem), pass it directly to GDAL.
		// MultiFileReader doesn't understand /vsizip/, /vsicurl/, etc. and silently returns
		// an empty file list, causing st_read_meta to return 0 rows.
		if (StringUtil::StartsWith(file_name, "/vsi")) {
			vector<OpenFileInfo> files;
			files.emplace_back(file_name);
			return make_uniq_base<FunctionData, BindData>(std::move(files));
		}

		const auto mfreader = MultiFileReader::Create(input.table_function);
		const auto mflist = mfreader->CreateFileList(context, input.inputs[0], FileGlobOptions::ALLOW_EMPTY);
		return make_uniq_base<FunctionData, BindData>(mflist->GetAllFiles());
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init
	//------------------------------------------------------------------------------------------------------------------
	struct State final : GlobalTableFunctionState {
		idx_t current_idx;
		explicit State() : current_idx(0) {
		}
	};

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		return make_uniq_base<GlobalTableFunctionState, State>();
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
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
			                                Value::LIST(GEOMETRY_FIELD_TYPE, std::move(geometry_fields)));

			vector<Value> standard_fields;
			for (const auto &field : layer->GetLayerDefn()->GetFields()) {
				child_list_t<Value> standard_field_value_fields;
				standard_field_value_fields.emplace_back("name", Value(field->GetNameRef()));
				standard_field_value_fields.emplace_back("type", Value(OGR_GetFieldTypeName(field->GetType())));
				standard_field_value_fields.emplace_back("subtype",
				                                         Value(OGR_GetFieldSubTypeName(field->GetSubType())));
				standard_field_value_fields.emplace_back("nullable", Value(field->IsNullable()));
				standard_field_value_fields.emplace_back("unique", Value(field->IsUnique()));
				standard_field_value_fields.emplace_back("width", Value(field->GetWidth()));
				standard_field_value_fields.emplace_back("precision", Value(field->GetPrecision()));
				standard_fields.push_back(Value::STRUCT(standard_field_value_fields));
			}
			layer_value_fields.emplace_back("fields", Value::LIST(STANDARD_FIELD_TYPE, std::move(standard_fields)));

			layer_values.push_back(Value::STRUCT(layer_value_fields));
		}

		return Value::LIST(LAYER_TYPE, std::move(layer_values));
	}

	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		auto &bind_data = input.bind_data->Cast<BindData>();
		auto &state = input.global_state->Cast<State>();

		const auto remaining = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind_data.file_names.size() - state.current_idx);
		auto output_idx = 0;

		for (idx_t in_idx = 0; in_idx < remaining; in_idx++, state.current_idx++) {
			auto &file = bind_data.file_names[state.current_idx];
			auto prefixed_file_name = GDALClientContextState::GetOrCreate(context).GetPrefix(file.path);

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

		output.SetCardinality(output_idx);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------
	// static constexpr DocTag DOC_TAGS[] = {{"ext", "spatial"}};

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

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		const TableFunction func("ST_Read_Meta", {LogicalType::VARCHAR}, Execute, Bind, Init);
		loader.RegisterFunction(MultiFileReader::CreateFunctionSet(func));

		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "spatial");
		FunctionBuilder::AddTableFunctionDocs(loader, "ST_Read_Meta", DESCRIPTION, EXAMPLE, tags);
	}
};

//======================================================================================================================
// ST_Drivers
//======================================================================================================================

struct ST_Drivers {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	struct BindData final : TableFunctionData {
		idx_t driver_count;
		explicit BindData(const idx_t driver_count_p) : driver_count(driver_count_p) {
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<string> &names) {

		return_types.emplace_back(LogicalType::VARCHAR);
		return_types.emplace_back(LogicalType::VARCHAR);
		return_types.emplace_back(LogicalType::BOOLEAN);
		return_types.emplace_back(LogicalType::BOOLEAN);
		return_types.emplace_back(LogicalType::BOOLEAN);
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("short_name");
		names.emplace_back("long_name");
		names.emplace_back("can_create");
		names.emplace_back("can_copy");
		names.emplace_back("can_open");
		names.emplace_back("help_url");

		return make_uniq_base<FunctionData, BindData>(GDALGetDriverCount());
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init
	//------------------------------------------------------------------------------------------------------------------
	struct State final : GlobalTableFunctionState {
		idx_t current_idx;
		explicit State() : current_idx(0) {
		}
	};

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		return make_uniq_base<GlobalTableFunctionState, State>();
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------
	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		auto &state = input.global_state->Cast<State>();
		auto &bind_data = input.bind_data->Cast<BindData>();

		idx_t count = 0;
		auto next_idx = MinValue<idx_t>(state.current_idx + STANDARD_VECTOR_SIZE, bind_data.driver_count);

		for (; state.current_idx < next_idx; state.current_idx++) {
			auto driver = GDALGetDriver(static_cast<int>(state.current_idx));

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
		output.SetCardinality(count);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------

	// static constexpr DocTag DOC_TAGS[] = {{"ext", "spatial"}};

	static constexpr auto DESCRIPTION = R"(
		Returns the list of supported GDAL drivers and file formats

		Note that far from all of these drivers have been tested properly.
		Some may require additional options to be passed to work as expected.
		If you run into any issues please first consult the [consult the GDAL docs](https://gdal.org/drivers/vector/index.html).
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT * FROM ST_Drivers();
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		const TableFunction func("ST_Drivers", {}, Execute, Bind, Init);
		loader.RegisterFunction(func);

		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "spatial");
		FunctionBuilder::AddTableFunctionDocs(loader, "ST_Drivers", DESCRIPTION, EXAMPLE, tags);
	}
};

//======================================================================================================================
// ST_Write
//======================================================================================================================
// TODO: This currently uses slow "Value" row-by-row conversions. GDAL now supports writing through arrow, so we should
// move into using that in the future.

struct ST_Write {

	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------
	struct BindData final : TableFunctionData {

		string file_path;
		vector<LogicalType> field_sql_types;
		vector<string> field_names;
		string driver_name;
		string layer_name;
		CPLStringList dataset_creation_options;
		CPLStringList layer_creation_options;
		string target_srs;
		OGRwkbGeometryType geometry_type = wkbUnknown;

		BindData(string file_path, vector<LogicalType> field_sql_types, vector<string> field_names)
		    : file_path(std::move(file_path)), field_sql_types(std::move(field_sql_types)),
		      field_names(std::move(field_names)) {
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, CopyFunctionBindInput &input,
	                                     const vector<string> &names, const vector<LogicalType> &sql_types) {

		auto bind_data = make_uniq<BindData>(input.info.file_path, sql_types, names);

		// check all the options in the copy info
		// and set
		for (auto &option : input.info.options) {
			if (StringUtil::Upper(option.first) == "DRIVER") {
				auto set = option.second.front();
				if (set.type().id() == LogicalTypeId::VARCHAR) {
					bind_data->driver_name = set.GetValue<string>();
				} else {
					throw BinderException("Driver name must be a string");
				}
			} else if (StringUtil::Upper(option.first) == "LAYER_NAME") {
				auto set = option.second.front();
				if (set.type().id() == LogicalTypeId::VARCHAR) {
					bind_data->layer_name = set.GetValue<string>();
				} else {
					throw BinderException("Layer name must be a string");
				}
			} else if (StringUtil::Upper(option.first) == "LAYER_CREATION_OPTIONS") {
				auto set = option.second;
				for (auto &s : set) {
					if (s.type().id() != LogicalTypeId::VARCHAR) {
						throw BinderException("Layer creation options must be strings");
					}
					auto str = s.GetValue<string>();
					bind_data->layer_creation_options.AddString(str.c_str());
				}
			} else if (StringUtil::Upper(option.first) == "DATASET_CREATION_OPTIONS") {
				auto set = option.second;
				for (auto &s : set) {
					if (s.type().id() != LogicalTypeId::VARCHAR) {
						throw BinderException("Dataset creation options must be strings");
					}
					auto str = s.GetValue<string>();
					bind_data->dataset_creation_options.AddString(str.c_str());
				}
			} else if (StringUtil::Upper(option.first) == "GEOMETRY_TYPE") {
				auto &set = option.second.front();
				if (set.type().id() == LogicalTypeId::VARCHAR) {
					auto type = set.GetValue<string>();
					if (StringUtil::CIEquals(type, "POINT")) {
						bind_data->geometry_type = wkbPoint;
					} else if (StringUtil::CIEquals(type, "LINESTRING")) {
						bind_data->geometry_type = wkbLineString;
					} else if (StringUtil::CIEquals(type, "POLYGON")) {
						bind_data->geometry_type = wkbPolygon;
					} else if (StringUtil::CIEquals(type, "MULTIPOINT")) {
						bind_data->geometry_type = wkbMultiPoint;
					} else if (StringUtil::CIEquals(type, "MULTILINESTRING")) {
						bind_data->geometry_type = wkbMultiLineString;
					} else if (StringUtil::CIEquals(type, "MULTIPOLYGON")) {
						bind_data->geometry_type = wkbMultiPolygon;
					} else if (StringUtil::CIEquals(type, "GEOMETRYCOLLECTION")) {
						bind_data->geometry_type = wkbGeometryCollection;
					} else {
						throw BinderException("Unknown geometry type '%s', expected one of 'POINT', 'LINESTRING', "
						                      "'POLYGON', 'MULTIPOINT', "
						                      "'MULTILINESTRING', 'MULTIPOLYGON', 'GEOMETRYCOLLECTION'",
						                      type);
					}
				} else {
					throw BinderException("Geometry type must be a string");
				}
			} else if (StringUtil::Upper(option.first) == "SRS") {
				auto &set = option.second.front();
				if (set.type().id() == LogicalTypeId::VARCHAR) {
					bind_data->target_srs = set.GetValue<string>();
				} else {
					throw BinderException("SRS must be a string");
				}
			} else {
				throw BinderException("Unknown option '%s'", option.first);
			}
			// save dataset open options.. i guess?
		}

		if (bind_data->driver_name.empty()) {
			throw BinderException("Driver name must be specified");
		}

		if (bind_data->layer_name.empty()) {
			// Default to the base name of the file
			auto &fs = FileSystem::GetFileSystem(context);
			bind_data->layer_name = fs.ExtractBaseName(bind_data->file_path);
		}

		auto driver = GetGDALDriverManager()->GetDriverByName(bind_data->driver_name.c_str());
		if (!driver) {
			throw BinderException("Unknown driver '%s'", bind_data->driver_name);
		}

		// Try get the file extension from the driver
		auto file_ext = driver->GetMetadataItem(GDAL_DMD_EXTENSION);
		if (file_ext) {
			input.file_extension = file_ext;
		} else {
			// Space separated list of file extensions
			auto file_exts = driver->GetMetadataItem(GDAL_DMD_EXTENSIONS);
			if (file_exts) {
				auto exts = StringUtil::Split(file_exts, ' ');
				if (!exts.empty()) {
					input.file_extension = exts[0];
				}
			}
		}

		// Driver specific checks
		if (bind_data->driver_name == "OpenFileGDB" && bind_data->geometry_type == wkbUnknown) {
			throw BinderException("OpenFileGDB requires 'GEOMETRY_TYPE' parameter to be set when writing!");
		}

		return std::move(bind_data);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Global State
	//------------------------------------------------------------------------------------------------------------------
	struct GlobalState final : GlobalFunctionData {
		mutex lock;
		GDALDatasetUniquePtr dataset;
		OGRLayer *layer;
		vector<unique_ptr<OGRFieldDefn>> field_defs;

		GlobalState(GDALDatasetUniquePtr dataset, OGRLayer *layer, vector<unique_ptr<OGRFieldDefn>> field_defs)
		    : dataset(std::move(dataset)), layer(layer), field_defs(std::move(field_defs)) {
		}
	};

	static bool IsGeometryType(const LogicalType &type) {
		return type == GeoTypes::WKB_BLOB() || type == GeoTypes::POINT_2D() || type == GeoTypes::GEOMETRY();
	}

	static unique_ptr<OGRFieldDefn> OGRFieldTypeFromLogicalType(const string &name, const LogicalType &type) {
		// TODO: Set OGRFieldSubType for integers and integer lists
		// TODO: Set string width?

		switch (type.id()) {
		case LogicalTypeId::BOOLEAN: {
			auto field = make_uniq<OGRFieldDefn>(name.c_str(), OFTInteger);
			field->SetSubType(OFSTBoolean);
			return field;
		}
		case LogicalTypeId::TINYINT: {
			// There is no subtype for byte?
			return make_uniq<OGRFieldDefn>(name.c_str(), OFTInteger);
		}
		case LogicalTypeId::SMALLINT: {
			auto field = make_uniq<OGRFieldDefn>(name.c_str(), OFTInteger);
			field->SetSubType(OFSTInt16);
			return field;
		}
		case LogicalTypeId::INTEGER: {
			return make_uniq<OGRFieldDefn>(name.c_str(), OFTInteger);
		}
		case LogicalTypeId::BIGINT:
			return make_uniq<OGRFieldDefn>(name.c_str(), OFTInteger64);
		case LogicalTypeId::FLOAT: {
			auto field = make_uniq<OGRFieldDefn>(name.c_str(), OFTReal);
			field->SetSubType(OFSTFloat32);
			return field;
		}
		case LogicalTypeId::DOUBLE:
			return make_uniq<OGRFieldDefn>(name.c_str(), OFTReal);
		case LogicalTypeId::VARCHAR:
			return make_uniq<OGRFieldDefn>(name.c_str(), OFTString);
		case LogicalTypeId::BLOB:
			return make_uniq<OGRFieldDefn>(name.c_str(), OFTBinary);
		case LogicalTypeId::DATE:
			return make_uniq<OGRFieldDefn>(name.c_str(), OFTDate);
		case LogicalTypeId::TIME:
			return make_uniq<OGRFieldDefn>(name.c_str(), OFTTime);
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_TZ:
			return make_uniq<OGRFieldDefn>(name.c_str(), OFTDateTime);
		case LogicalTypeId::LIST: {
			auto child_type = ListType::GetChildType(type);
			switch (child_type.id()) {
			case LogicalTypeId::BOOLEAN: {
				auto field = make_uniq<OGRFieldDefn>(name.c_str(), OFTIntegerList);
				field->SetSubType(OFSTBoolean);
				return field;
			}
			case LogicalTypeId::TINYINT: {
				// There is no subtype for byte?
				return make_uniq<OGRFieldDefn>(name.c_str(), OFTIntegerList);
			}
			case LogicalTypeId::SMALLINT: {
				auto field = make_uniq<OGRFieldDefn>(name.c_str(), OFTIntegerList);
				field->SetSubType(OFSTInt16);
				return field;
			}
			case LogicalTypeId::INTEGER:
				return make_uniq<OGRFieldDefn>(name.c_str(), OFTIntegerList);
			case LogicalTypeId::BIGINT:
				return make_uniq<OGRFieldDefn>(name.c_str(), OFTInteger64List);
			case LogicalTypeId::FLOAT: {
				auto field = make_uniq<OGRFieldDefn>(name.c_str(), OFTRealList);
				field->SetSubType(OFSTFloat32);
				return field;
			}
			case LogicalTypeId::DOUBLE:
				return make_uniq<OGRFieldDefn>(name.c_str(), OFTRealList);
			case LogicalTypeId::VARCHAR:
				return make_uniq<OGRFieldDefn>(name.c_str(), OFTStringList);
			default:
				throw NotImplementedException("Unsupported type for OGR: %s", type.ToString());
			}
		}
		default:
			throw NotImplementedException("Unsupported type for OGR: %s", type.ToString());
		}
	}

	static unique_ptr<GlobalFunctionData> InitGlobal(ClientContext &context, FunctionData &bind_data,
	                                                 const string &file_path) {

		auto &gdal_data = bind_data.Cast<BindData>();
		GDALDriver *driver = GetGDALDriverManager()->GetDriverByName(gdal_data.driver_name.c_str());
		if (!driver) {
			throw IOException("Could not open driver");
		}

		// Create the dataset
		auto &client_ctx = GDALClientContextState::GetOrCreate(context);
		auto prefixed_path = client_ctx.GetPrefix(file_path);
		auto dataset = GDALDatasetUniquePtr(
		    driver->Create(prefixed_path.c_str(), 0, 0, 0, GDT_Unknown, gdal_data.dataset_creation_options));
		if (!dataset) {
			throw IOException("Could not open dataset");
		}

		// Set the SRS if provided
		OGRSpatialReference srs;
		if (!gdal_data.target_srs.empty()) {
			srs.SetFromUserInput(gdal_data.target_srs.c_str());
		}
		// Not all GDAL drivers check if the SRS is empty (cough cough GeoJSONSeq)
		// so we have to pass nullptr if we want the default behavior.
		OGRSpatialReference *srs_ptr = gdal_data.target_srs.empty() ? nullptr : &srs;

		auto layer = dataset->CreateLayer(gdal_data.layer_name.c_str(), srs_ptr, gdal_data.geometry_type,
		                                  gdal_data.layer_creation_options);
		if (!layer) {
			throw IOException("Could not create layer");
		}

		// Create the layer field definitions
		idx_t geometry_field_count = 0;
		vector<unique_ptr<OGRFieldDefn>> field_defs;
		for (idx_t i = 0; i < gdal_data.field_names.size(); i++) {
			auto &name = gdal_data.field_names[i];
			auto &type = gdal_data.field_sql_types[i];

			if (IsGeometryType(type)) {
				geometry_field_count++;
				if (geometry_field_count > 1) {
					throw NotImplementedException("Multiple geometry fields not supported yet");
				}
			} else {
				auto field = OGRFieldTypeFromLogicalType(name, type);
				if (layer->CreateField(field.get()) != OGRERR_NONE) {
					throw IOException("Could not create attribute field");
				}
				// TODO: ^ Like we do here vvv
				field_defs.push_back(std::move(field));
			}
		}
		auto global_data = make_uniq<GlobalState>(std::move(dataset), layer, std::move(field_defs));

		return std::move(global_data);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Local State
	//------------------------------------------------------------------------------------------------------------------
	struct LocalState final : public LocalFunctionData {
		ArenaAllocator arena;
		explicit LocalState(ClientContext &context) : arena(BufferAllocator::Get(context)) {
		}
	};

	static unique_ptr<LocalFunctionData> InitLocal(ExecutionContext &context, FunctionData &bind_data) {
		auto local_data = make_uniq<LocalState>(context.client);
		return std::move(local_data);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Sink
	//------------------------------------------------------------------------------------------------------------------
	static OGRGeometryUniquePtr OGRGeometryFromValue(const LogicalType &type, const Value &value,
	                                                 ArenaAllocator &arena) {
		if (value.IsNull()) {
			return nullptr;
		}

		if (type == GeoTypes::WKB_BLOB()) {
			const auto str = value.GetValueUnsafe<string_t>();
			OGRGeometry *ptr;
			size_t consumed;
			const auto ok = OGRGeometryFactory::createFromWkb(str.GetDataUnsafe(), nullptr, &ptr, str.GetSize(),
			                                                  wkbVariantIso, consumed);

			if (ok != OGRERR_NONE) {
				throw IOException("Could not parse WKB");
			}
			return OGRGeometryUniquePtr(ptr);
		}

		if (type == GeoTypes::GEOMETRY()) {
			const auto blob = value.GetValueUnsafe<string_t>();
			uint32_t size;
			const auto wkb = WKBWriter::Write(blob, &size, arena);
			OGRGeometry *ptr;
			const auto ok = OGRGeometryFactory::createFromWkb(wkb, nullptr, &ptr, size, wkbVariantIso);
			if (ok != OGRERR_NONE) {
				throw IOException("Could not parse WKB");
			}
			return OGRGeometryUniquePtr(ptr);
		}

		if (type == GeoTypes::POINT_2D()) {
			auto children = StructValue::GetChildren(value);
			auto x = children[0].GetValue<double>();
			auto y = children[1].GetValue<double>();
			auto ogr_point = new OGRPoint(x, y);
			return OGRGeometryUniquePtr(ogr_point);
		}

		throw NotImplementedException("Unsupported geometry type");
	}

	static void SetOgrFieldFromValue(OGRFeature *feature, int field_idx, const LogicalType &type, const Value &value) {
		// TODO: Set field by index always instead of by name for performance.
		if (value.IsNull()) {
			feature->SetFieldNull(field_idx);
			return;
		}
		switch (type.id()) {
		case LogicalTypeId::BOOLEAN:
			feature->SetField(field_idx, value.GetValue<bool>());
			break;
		case LogicalTypeId::TINYINT:
			feature->SetField(field_idx, value.GetValue<int8_t>());
			break;
		case LogicalTypeId::SMALLINT:
			feature->SetField(field_idx, value.GetValue<int16_t>());
			break;
		case LogicalTypeId::INTEGER:
			feature->SetField(field_idx, value.GetValue<int32_t>());
			break;
		case LogicalTypeId::BIGINT:
			feature->SetField(field_idx, (GIntBig)value.GetValue<int64_t>());
			break;
		case LogicalTypeId::FLOAT:
			feature->SetField(field_idx, value.GetValue<float>());
			break;
		case LogicalTypeId::DOUBLE:
			feature->SetField(field_idx, value.GetValue<double>());
			break;
		case LogicalTypeId::VARCHAR:
		case LogicalTypeId::BLOB: {
			auto str = value.GetValueUnsafe<string_t>();
			feature->SetField(field_idx, (int)str.GetSize(), str.GetDataUnsafe());
		} break;
		case LogicalTypeId::DATE: {
			auto date = value.GetValueUnsafe<date_t>();
			auto year = Date::ExtractYear(date);
			auto month = Date::ExtractMonth(date);
			auto day = Date::ExtractDay(date);
			feature->SetField(field_idx, year, month, day, 0, 0, 0, 0);
		} break;
		case LogicalTypeId::TIME: {
			auto time = value.GetValueUnsafe<dtime_t>();
			auto hour = static_cast<int>(time.micros / Interval::MICROS_PER_HOUR);
			auto minute = static_cast<int>((time.micros % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE);
			auto second = static_cast<float>(static_cast<double>(time.micros % Interval::MICROS_PER_MINUTE) /
			                                 static_cast<double>(Interval::MICROS_PER_SEC));
			feature->SetField(field_idx, 0, 0, 0, hour, minute, second, 0);
		} break;
		case LogicalTypeId::TIMESTAMP: {
			auto timestamp = value.GetValueUnsafe<timestamp_t>();
			auto date = Timestamp::GetDate(timestamp);
			auto time = Timestamp::GetTime(timestamp);
			auto year = Date::ExtractYear(date);
			auto month = Date::ExtractMonth(date);
			auto day = Date::ExtractDay(date);
			auto hour = static_cast<int>((time.micros % Interval::MICROS_PER_DAY) / Interval::MICROS_PER_HOUR);
			auto minute = static_cast<int>((time.micros % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE);
			auto second = static_cast<float>(static_cast<double>(time.micros % Interval::MICROS_PER_MINUTE) /
			                                 static_cast<double>(Interval::MICROS_PER_SEC));
			feature->SetField(field_idx, year, month, day, hour, minute, second, 0);
		} break;
		case LogicalTypeId::TIMESTAMP_NS: {
			auto timestamp = value.GetValueUnsafe<timestamp_t>();
			timestamp = Timestamp::FromEpochNanoSeconds(timestamp.value);
			auto date = Timestamp::GetDate(timestamp);
			auto time = Timestamp::GetTime(timestamp);
			auto year = Date::ExtractYear(date);
			auto month = Date::ExtractMonth(date);
			auto day = Date::ExtractDay(date);
			auto hour = static_cast<int>((time.micros % Interval::MICROS_PER_DAY) / Interval::MICROS_PER_HOUR);
			auto minute = static_cast<int>((time.micros % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE);
			auto second = static_cast<float>(static_cast<double>(time.micros % Interval::MICROS_PER_MINUTE) /
			                                 static_cast<double>(Interval::MICROS_PER_SEC));
			feature->SetField(field_idx, year, month, day, hour, minute, second, 0);
		} break;
		case LogicalTypeId::TIMESTAMP_MS: {
			auto timestamp = value.GetValueUnsafe<timestamp_t>();
			timestamp = Timestamp::FromEpochMs(timestamp.value);
			auto date = Timestamp::GetDate(timestamp);
			auto time = Timestamp::GetTime(timestamp);
			auto year = Date::ExtractYear(date);
			auto month = Date::ExtractMonth(date);
			auto day = Date::ExtractDay(date);
			auto hour = static_cast<int>((time.micros % Interval::MICROS_PER_DAY) / Interval::MICROS_PER_HOUR);
			auto minute = static_cast<int>((time.micros % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE);
			auto second = static_cast<float>(static_cast<double>(time.micros % Interval::MICROS_PER_MINUTE) /
			                                 static_cast<double>(Interval::MICROS_PER_SEC));
			feature->SetField(field_idx, year, month, day, hour, minute, second, 0);
		} break;
		case LogicalTypeId::TIMESTAMP_SEC: {
			auto timestamp = value.GetValueUnsafe<timestamp_t>();
			timestamp = Timestamp::FromEpochSeconds(timestamp.value);
			auto date = Timestamp::GetDate(timestamp);
			auto time = Timestamp::GetTime(timestamp);
			auto year = Date::ExtractYear(date);
			auto month = Date::ExtractMonth(date);
			auto day = Date::ExtractDay(date);
			auto hour = static_cast<int>((time.micros % Interval::MICROS_PER_DAY) / Interval::MICROS_PER_HOUR);
			auto minute = static_cast<int>((time.micros % Interval::MICROS_PER_HOUR) / Interval::MICROS_PER_MINUTE);
			auto second = static_cast<float>(static_cast<double>(time.micros % Interval::MICROS_PER_MINUTE) /
			                                 static_cast<double>(Interval::MICROS_PER_SEC));
			feature->SetField(field_idx, year, month, day, hour, minute, second, 0);
		} break;
		case LogicalTypeId::TIMESTAMP_TZ: {
			// Not sure what to with the timezone, just let GDAL parse it?
			auto timestamp = value.GetValueUnsafe<timestamp_t>();
			auto time_str = Timestamp::ToString(timestamp);
			feature->SetField(field_idx, time_str.c_str());
		} break;
		default:
			// TODO: Handle list types
			throw NotImplementedException("Unsupported field type");
		}
	}

	static void Sink(ExecutionContext &context, FunctionData &bdata, GlobalFunctionData &gstate,
	                 LocalFunctionData &lstate, DataChunk &input) {

		auto &bind_data = bdata.Cast<BindData>();
		auto &global_state = gstate.Cast<GlobalState>();
		auto &local_state = lstate.Cast<LocalState>();
		local_state.arena.Reset();

		lock_guard<mutex> d_lock(global_state.lock);
		auto layer = global_state.layer;

		// Create the feature
		input.Flatten();
		for (idx_t row_idx = 0; row_idx < input.size(); row_idx++) {

			auto feature = OGRFeatureUniquePtr(OGRFeature::CreateFeature(layer->GetLayerDefn()));

			// Geometry fields do not count towards the field index, so we need to keep track of them separately.
			idx_t field_idx = 0;
			for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
				auto &type = bind_data.field_sql_types[col_idx];
				auto value = input.GetValue(col_idx, row_idx);

				if (IsGeometryType(type)) {
					// TODO: check how many geometry fields there are and use the correct one.
					auto geom = OGRGeometryFromValue(type, value, local_state.arena);
					if (geom && bind_data.geometry_type != wkbUnknown &&
					    geom->getGeometryType() != bind_data.geometry_type) {
						auto got_name = StringUtil::Replace(
						    StringUtil::Upper(OGRGeometryTypeToName(geom->getGeometryType())), " ", "");
						auto expected_name = StringUtil::Replace(
						    StringUtil::Upper(OGRGeometryTypeToName(bind_data.geometry_type)), " ", "");
						throw InvalidInputException(
						    "Expected all geometries to be of type '%s', but got one of type '%s'", expected_name,
						    got_name);
					}

					if (feature->SetGeometryDirectly(geom.release()) != OGRERR_NONE) {
						throw IOException("Could not set geometry");
					}
				} else {
					SetOgrFieldFromValue(feature.get(), static_cast<int>(field_idx), type, value);
					field_idx++;
				}
			}
			if (layer->CreateFeature(feature.get()) != OGRERR_NONE) {
				throw IOException("Could not create feature");
			}
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Combine
	//------------------------------------------------------------------------------------------------------------------
	static void Combine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
	                    LocalFunctionData &lstate) {
	}

	//------------------------------------------------------------------------------------------------------------------
	// Finalize
	//------------------------------------------------------------------------------------------------------------------
	static void Finalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
		const auto &global_state = gstate.Cast<GlobalState>();
		global_state.dataset->FlushCache();
		global_state.dataset->Close();
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------
	static void Register(ExtensionLoader &loader) {
		CopyFunction info("GDAL");
		info.copy_to_bind = Bind;
		info.copy_to_initialize_local = InitLocal;
		info.copy_to_initialize_global = InitGlobal;
		info.copy_to_sink = Sink;
		info.copy_to_combine = Combine;
		info.copy_to_finalize = Finalize;
		info.extension = "gdal";
		loader.RegisterFunction(info);
	}
};

} // namespace

//######################################################################################################################
// Register Module
//######################################################################################################################
void RegisterGDALModule(ExtensionLoader &loader) {

	// Load GDAL (once)
	static std::once_flag loaded;
	std::call_once(loaded, [&]() {
		// Register all embedded drivers (dont go looking for plugins)
		OGRRegisterAllInternal();

		// Set GDAL error handler
		CPLSetErrorHandler([](CPLErr e, int code, const char *raw_msg) {
			// DuckDB doesn't do warnings, so we only throw on errors
			if (e != CE_Failure && e != CE_Fatal) {
				return;
			}

			// If the error contains a /vsiduckdb-<uuid>/ prefix,
			// try to strip it off to make the errors more readable
			auto msg = string(raw_msg);
			auto path_pos = msg.find("/vsiduckdb-");
			if (path_pos != string::npos) {
				// We found a path, strip it off
				msg.erase(path_pos, 48);
			}

			switch (code) {
			case CPLE_NoWriteAccess:
				throw PermissionException("GDAL Error (%d): %s", code, msg);
			case CPLE_UserInterrupt:
				throw InterruptException();
			case CPLE_OutOfMemory:
				throw OutOfMemoryException("GDAL Error (%d): %s", code, msg);
			case CPLE_NotSupported:
				throw NotImplementedException("GDAL Error (%d): %s", code, msg);
			case CPLE_AssertionFailed:
			case CPLE_ObjectNull:
				throw InternalException("GDAL Error (%d): %s", code, msg);
			case CPLE_IllegalArg:
				throw InvalidInputException("GDAL Error (%d): %s", code, msg);
			case CPLE_AppDefined:
			case CPLE_HttpResponse:
			case CPLE_FileIO:
			case CPLE_OpenFailed:
			default:
				throw IOException("GDAL Error (%d): %s", code, msg);
			}
		});
	});

	ST_Read::Register(loader);
	ST_Read_Meta::Register(loader);
	ST_Drivers::Register(loader);
	ST_Write::Register(loader);
}

} // namespace duckdb
