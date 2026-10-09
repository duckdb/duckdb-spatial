#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "spatial/modules/osm/osm_module.hpp"

#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "protozero/pbf_reader.hpp"
#include "spatial/spatial_types.hpp"
#include "zlib.h"

#include <spatial/util/function_builder.hpp>

namespace duckdb {

namespace {
//------------------------------------------------------------------------------
// Utils
//------------------------------------------------------------------------------

namespace pz = protozero;

int32_t ReadInt32BigEndian(data_ptr_t ptr) {
	return (ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3];
}

//------------------------------------------------------------------------------
// OSM Table Function
//------------------------------------------------------------------------------

struct BindData final : TableFunctionData {
	string file_name;

	explicit BindData(string file_name) : file_name(std::move(file_name)) {
	}
};

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {

	// Create an enum type for all osm kinds
	vector<string_t> enum_values = {"node", "way", "relation", "changeset"};
	auto varchar_vector = Vector(LogicalType::VARCHAR, enum_values.size());
	auto varchar_data = FlatVector::GetDataMutable<string_t>(varchar_vector);
	for (idx_t i = 0; i < enum_values.size(); i++) {
		auto str = enum_values[i];
		varchar_data[i] = str.IsInlined() ? str : StringVector::AddString(varchar_vector, str);
	}

	// Set return types
	return_types.push_back(LogicalType::ENUM("OSM_ENTITY_TYPE", varchar_vector, enum_values.size()));
	names.push_back("kind");

	return_types.push_back(LogicalType::BIGINT);
	names.push_back("id");

	return_types.push_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
	names.push_back("tags");

	return_types.push_back(LogicalType::LIST(LogicalType::BIGINT));
	names.push_back("refs");

	return_types.push_back(LogicalType::DOUBLE);
	names.push_back("lat");

	return_types.push_back(LogicalType::DOUBLE);
	names.push_back("lon");

	return_types.push_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.push_back("ref_roles");

	// Create an enum type for the member kind
	vector<string_t> member_enum_values = {"node", "way", "relation"};
	auto member_varchar_vector = Vector(LogicalType::VARCHAR, member_enum_values.size());
	auto member_varchar_data = FlatVector::GetDataMutable<string_t>(member_varchar_vector);
	for (idx_t i = 0; i < member_enum_values.size(); i++) {
		auto str = member_enum_values[i];
		member_varchar_data[i] = str.IsInlined() ? str : StringVector::AddString(member_varchar_vector, str);
	}

	return_types.push_back(
	    LogicalType::LIST(LogicalType::ENUM("OSM_REF_TYPE", member_varchar_vector, member_enum_values.size())));
	names.push_back("ref_types");

	// Create bind data
	auto file_name = StringValue::Get(input.inputs[0]);
	auto result = make_uniq<BindData>(file_name);
	return std::move(result);
}

enum class FileBlockType { Header, Data };

struct OsmBlob {
	FileBlockType type;
	AllocatedData data;
	idx_t size;
	idx_t blob_idx;

	explicit OsmBlob(FileBlockType type, AllocatedData data, idx_t size, idx_t blob_idx)
	    : type(type), data(std::move(data)), size(size), blob_idx(blob_idx) {
	}
};

struct FileBlock {
	FileBlockType type; // type of block
	AllocatedData data; // raw or decompressed data
	idx_t size;         // size of the data
	idx_t block_idx;    // index of the block in the file

	explicit FileBlock(FileBlockType type, AllocatedData data, idx_t size, idx_t block_idx)
	    : type(type), data(std::move(data)), size(size), block_idx(block_idx) {
	}
};

unique_ptr<FileBlock> DecompressBlob(ClientContext &context, OsmBlob &blob) {

	auto &buffer_manager = BufferManager::GetBufferManager(context);
	pz::pbf_reader reader(reinterpret_cast<const char *>(blob.data.get()), blob.size);

	// TODO: For now we assume they are all zlib compressed
	reader.next(2);
	auto blob_uncompressed_size = reader.get_int32();
	reader.next(3);
	auto view = reader.get_view();

	auto uncompressed_handle = buffer_manager.GetBufferAllocator().Allocate(blob_uncompressed_size);
	auto uncompressed_ptr = uncompressed_handle.get();

	z_stream zstream = {};
	zstream.avail_in = view.size();
	zstream.next_in = (Bytef *)view.data();
	zstream.avail_out = blob_uncompressed_size;
	zstream.next_out = (Bytef *)uncompressed_ptr;
	auto ok = inflateInit(&zstream);
	if (ok != Z_OK) {
		throw ParserException("Failed to initialize zlib");
	}
	ok = inflate(&zstream, Z_FINISH);
	if (ok != Z_STREAM_END) {
		throw ParserException("Failed to inflate zlib");
	}
	ok = inflateEnd(&zstream);
	// Cool, we have the uncompressed data

	return make_uniq<FileBlock>(blob.type, std::move(uncompressed_handle), blob_uncompressed_size, blob.blob_idx);
};

class GlobalState final : public GlobalTableFunctionState {
	mutex lock;
	unique_ptr<FileHandle> handle;
	idx_t file_size;
	idx_t offset;
	bool done;
	idx_t blob_index;
	atomic<idx_t> bytes_read;
	idx_t max_threads;

public:
	GlobalState(unique_ptr<FileHandle> handle, idx_t file_size, idx_t max_threads)
	    : handle(std::move(handle)), file_size(file_size), offset(0), done(false), blob_index(0), bytes_read(0),
	      max_threads(max_threads) {
	}

	double GetProgress() const {
		return 100 * ((double)bytes_read / (double)file_size);
	}

	idx_t MaxThreads() const override {
		return max_threads;
	}

	unique_ptr<OsmBlob> GetNextBlob(ClientContext &context) {
		lock_guard<mutex> glock(lock);

		if (done) {
			return nullptr;
		}
		if (offset >= file_size) {
			done = true;
			return nullptr;
		}

		auto &buffer_manager = BufferManager::GetBufferManager(context);

		// The format is a repeating sequence of:
		//    int4: length of the BlobHeader message in network byte order
		//    serialized BlobHeader message
		//    serialized Blob message (size is given in the header)

		// Read the length of the BlobHeader
		char header_length_be[4];
		handle->Read(header_length_be, sizeof(int32_t), offset);
		offset += sizeof(int32_t);

		const auto header_length = ReadInt32BigEndian(data_ptr_cast(header_length_be));

		// Sanity check
		if (offset + header_length > file_size) {
			throw ParserException("Unexpected end of file when reading BlobHeader");
		}

		// Read the BlobHeader
		auto header_buffer = buffer_manager.GetBufferAllocator().Allocate(header_length);
		handle->Read(header_buffer.get(), header_length, offset);
		offset += header_length;

		pz::pbf_reader reader(char_ptr_cast(header_buffer.get()), header_length);

		// 1 - type of the blob
		reader.next(1);
		auto type_str = reader.get_string();
		FileBlockType type;
		if (type_str == "OSMHeader") {
			type = FileBlockType::Header;
		} else if (type_str == "OSMData") {
			type = FileBlockType::Data;
		} else {
			throw ParserException("Unexpected fileblock type in Blob");
		}
		// 3 - size of the next blob
		reader.next(3);
		auto blob_length = reader.get_int32(); // size of the next blob

		// Sanity check
		if (offset + blob_length > file_size) {
			throw ParserException("Unexpected end of file when reading Blob");
		}

		// Read the Blob
		auto blob_buffer = buffer_manager.GetBufferAllocator().Allocate(blob_length);
		handle->Read(blob_buffer.get(), blob_length, offset);
		offset += blob_length;

		bytes_read = offset;

		return make_uniq<OsmBlob>(type, std::move(blob_buffer), blob_length, blob_index++);
	}
};

unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<BindData>();

	auto &fs = FileSystem::GetFileSystem(context);
	auto file_name = bind_data.file_name;

	auto handle = fs.OpenFile(file_name, FileFlags::FILE_FLAGS_READ | FileLockType::READ_LOCK);
	auto file_size = handle->GetFileSize();

	auto max_threads = context.db->NumberOfThreads();

	auto global_state = make_uniq<GlobalState>(std::move(handle), file_size, max_threads);

	// Read the first blob to get the header
	auto blob = global_state->GetNextBlob(context);
	if (blob->type != FileBlockType::Header) {
		throw ParserException("First blob in file is not a header");
	}

	return std::move(global_state);
}

struct LocalState final : LocalTableFunctionState {
	unique_ptr<FileBlock> block;
	vector<string> string_table;
	int32_t granularity;
	int64_t lat_offset;
	int64_t lon_offset;

	explicit LocalState(unique_ptr<FileBlock> block) : block(std::move(block)) {
		Reset();
	}

	void SetBlock(unique_ptr<FileBlock> block) {
		this->block = std::move(block);
		Reset();
	}

	void Reset() {
		string_table.clear();
		granularity = 100;
		lat_offset = 0;
		lon_offset = 0;

		block_reader = pz::pbf_reader(const_char_ptr_cast(block->data.get()), block->size);
		block_reader.next(1); // String table
		auto string_table_reader = block_reader.get_message();
		while (string_table_reader.next(1)) {
			string_table.push_back(string_table_reader.get_string());
		}

		// Need to read ahead without advancing block_reader
		auto reader_copy = block_reader;

		// Read the granularity and optional offsets
		if (reader_copy.next(17)) {
			granularity = reader_copy.get_int32();
		}
		if (reader_copy.next(19)) {
			lat_offset = reader_copy.get_int64();
		}
		if (reader_copy.next(20)) {
			lon_offset = reader_copy.get_int64();
		}

		state = ParseState::Block;
	}

	pz::pbf_reader block_reader;
	pz::pbf_reader group_reader;

	idx_t dense_node_index;
	vector<int64_t> dense_node_ids;
	vector<uint32_t> dense_node_tags;
	vector<list_entry_t> dense_node_tag_entries;
	vector<int64_t> dense_node_lats;
	vector<int64_t> dense_node_lons;

	enum class ParseState { Block, Group, DenseNodes, End };

	ParseState state = ParseState::Block;

	// Returns false if there is data left to read but we've reached the capacity
	// Returns true if block is empty and we are done
	bool TryRead(DataChunk &output, idx_t &index, idx_t capacity) {
		// Main finite state machine
		while (index < capacity) {
			switch (state) {
			case ParseState::Block:
				if (block_reader.next(2)) {
					group_reader = block_reader.get_message();
					state = ParseState::Group;
				} else {
					state = ParseState::End;
				}
				break;
			case ParseState::Group:
				if (group_reader.next()) {
					switch (group_reader.tag()) {
						// Nodes
					case 1: {
						ScanNode(output, index, capacity);
					} break;
						// Dense nodes
					case 2: {
						PrepareDenseNodes(output, index, capacity);
						state = ParseState::DenseNodes;
					} break;
						// Way
					case 3: {
						ScanWay(output, index, capacity);
					} break;
						// Relation
					case 4: {
						ScanRelation(output, index, capacity);
					} break;
						// Changeset
					case 5: {
						// Skip for now.
						group_reader.skip();
					} break;
					default: {
						group_reader.skip();
					} break;
					}
				} else {
					state = ParseState::Block;
				}
				break;
			case ParseState::DenseNodes: {
				auto done = ScanDenseNodes(output, index, capacity);
				if (done) {
					state = ParseState::Group;
				}
			} break;
			case ParseState::End:
			default:
				return true;
			}
		}
		return false;
	}

	void ScanNode(DataChunk &output, idx_t &index, idx_t capacity) {

		auto node = group_reader.get_message();

		pz::iterator_range<pz::const_varint_iterator<uint32_t>> key_iter;
		pz::iterator_range<pz::const_varint_iterator<uint32_t>> val_iter;

		while (node.next()) {
			switch (node.tag()) {
			case 1: { // ID
				auto id = node.get_int64();
				FlatVector::GetDataMutable<uint8_t>(output.data[0])[index] = 0;
				FlatVector::GetDataMutable<int64_t>(output.data[1])[index] = id;
			} break;
			case 2: { // Tag Keys
				key_iter = node.get_packed_uint32();
			} break;
			case 3: { // Tag Vals
				val_iter = node.get_packed_uint32();
			} break;
			case 8: { // Lat
				auto lat = node.get_sint64();
				FlatVector::GetDataMutable<double>(output.data[4])[index] = 0.000000001 * (lat_offset + (granularity * lat));
			} break;
			case 9: { // Lon
				auto lon = node.get_sint64();
				FlatVector::GetDataMutable<double>(output.data[5])[index] = 0.000000001 * (lon_offset + (granularity * lon));
			} break;
			default:
				node.skip();
			}
		}

		// Read tags
		if (!key_iter.empty() && !val_iter.empty()) {
			auto tag_count = key_iter.size();
			auto total_tags = ListVector::GetListSize(output.data[2]);
			ListVector::Reserve(output.data[2], total_tags + tag_count);
			ListVector::SetListSize(output.data[2], total_tags + tag_count);
			auto &tag_entry = FlatVector::GetDataMutable<list_entry_t>(output.data[2])[index];

			tag_entry.offset = total_tags;
			tag_entry.length = tag_count;

			auto &key_vector = MapVector::GetKeys(output.data[2]);
			auto &value_vector = MapVector::GetValues(output.data[2]);

			auto keys = key_iter.begin();
			auto vals = val_iter.begin();
			for (idx_t i = tag_entry.offset; i < tag_entry.offset + tag_count; i++) {
				FlatVector::GetDataMutable<string_t>(key_vector)[i] =
				    StringVector::AddString(key_vector, string_table[*keys++]);
				FlatVector::GetDataMutable<string_t>(value_vector)[i] =
				    StringVector::AddString(value_vector, string_table[*vals++]);
			}
		} else {
			FlatVector::SetNull(output.data[2], index, true);
		}

		// Node has no refs, ref_roles or ref_types
		FlatVector::SetNull(output.data[3], index, true);
		FlatVector::SetNull(output.data[6], index, true);
		FlatVector::SetNull(output.data[7], index, true);

		index++;
	}

	void PrepareDenseNodes(DataChunk &output, idx_t &index, idx_t capacity) {
		dense_node_index = 0;
		dense_node_ids.clear();
		dense_node_tags.clear();
		dense_node_tag_entries.clear();
		dense_node_lats.clear();
		dense_node_lons.clear();

		auto dense_nodes = group_reader.get_message();

		while (dense_nodes.next()) {
			switch (dense_nodes.tag()) {
			case 1: { // ID
				auto ids = dense_nodes.get_packed_sint64();
				int64_t last_id = 0;
				for (auto id : ids) {
					last_id += id;
					dense_node_ids.push_back(last_id);
				}
			} break;
			case 8: { // Lats
				auto lats = dense_nodes.get_packed_sint64();
				int64_t last_lat = 0;
				for (auto lat : lats) {
					last_lat += lat;
					dense_node_lats.push_back(last_lat);
				}
			} break;
			case 9: { // Lons
				auto lons = dense_nodes.get_packed_sint64();
				int64_t last_lon = 0;
				for (auto lon : lons) {
					last_lon += lon;
					dense_node_lons.push_back(last_lon);
				}
			} break;
			case 10: { // Tags
				auto tags = dense_nodes.get_packed_uint32();
				idx_t entry_offset = 0;
				for (auto tag : tags) {
					if (tag == 0) {
						auto len = dense_node_tags.size() - entry_offset;
						dense_node_tag_entries.push_back(list_entry_t {entry_offset, len});
						entry_offset = dense_node_tags.size();
					} else {
						dense_node_tags.push_back(tag);
					}
				}
			} break;
			default:
				dense_nodes.skip();
			}
		}
	}

	void ScanWay(DataChunk &output, idx_t &index, idx_t capacity) {
		auto way = group_reader.get_message();

		pz::iterator_range<pz::const_varint_iterator<uint32_t>> key_iter;
		pz::iterator_range<pz::const_varint_iterator<uint32_t>> val_iter;
		pz::iterator_range<pz::const_svarint_iterator<int64_t>> ref_iter;

		while (way.next()) {
			switch (way.tag()) {
			case 1: { // ID
				auto id = way.get_int64();
				FlatVector::GetDataMutable<uint8_t>(output.data[0])[index] = 1;
				FlatVector::GetDataMutable<int64_t>(output.data[1])[index] = id;
				FlatVector::SetNull(output.data[4], index, true);
				FlatVector::SetNull(output.data[5], index, true);
				FlatVector::SetNull(output.data[6], index, true);
				FlatVector::SetNull(output.data[7], index, true);
			} break;
			case 2: { // Tag Keys
				key_iter = way.get_packed_uint32();
			} break;
			case 3: { // Tag Vals
				val_iter = way.get_packed_uint32();
			} break;
			case 8: { // Refs
				ref_iter = way.get_packed_sint64();
			} break;
			default:
				way.skip();
			}
		}
		if (!key_iter.empty() && !val_iter.empty()) {
			auto tag_count = key_iter.size();
			auto total_tags = ListVector::GetListSize(output.data[2]);
			ListVector::Reserve(output.data[2], total_tags + tag_count);
			ListVector::SetListSize(output.data[2], total_tags + tag_count);
			auto &tag_entry = FlatVector::GetDataMutable<list_entry_t>(output.data[2])[index];

			tag_entry.offset = total_tags;
			tag_entry.length = tag_count;

			auto &key_vector = MapVector::GetKeys(output.data[2]);
			auto &value_vector = MapVector::GetValues(output.data[2]);

			auto keys = key_iter.begin();
			auto vals = val_iter.begin();
			for (idx_t i = tag_entry.offset; i < tag_entry.offset + tag_count; i++) {
				FlatVector::GetDataMutable<string_t>(key_vector)[i] =
				    StringVector::AddString(key_vector, string_table[*keys++]);
				FlatVector::GetDataMutable<string_t>(value_vector)[i] =
				    StringVector::AddString(value_vector, string_table[*vals++]);
			}
		} else {
			FlatVector::SetNull(output.data[2], index, true);
		}

		if (!ref_iter.empty()) {
			auto ref_count = ref_iter.size();
			auto total_refs = ListVector::GetListSize(output.data[3]);
			ListVector::Reserve(output.data[3], total_refs + ref_count);
			ListVector::SetListSize(output.data[3], total_refs + ref_count);
			auto &ref_entry = FlatVector::GetDataMutable<list_entry_t>(output.data[3])[index];
			auto &ref_vector = ListVector::GetChildMutable(output.data[3]);
			ref_entry.offset = total_refs;
			ref_entry.length = ref_count;

			auto ref_data = FlatVector::GetDataMutable<int64_t>(ref_vector);

			int64_t last_ref = 0;
			for (auto ref : ref_iter) {
				last_ref += ref;
				ref_data[total_refs++] = last_ref;
			}
		} else {
			FlatVector::SetNull(output.data[3], index, true);
		}

		index++;
	}

	void ScanRelation(DataChunk &output, idx_t &index, idx_t capacity) {
		auto relation = group_reader.get_message();

		pz::iterator_range<pz::const_varint_iterator<uint32_t>> key_iter;
		pz::iterator_range<pz::const_varint_iterator<uint32_t>> val_iter;
		pz::iterator_range<pz::const_varint_iterator<int32_t>> role_iter;
		pz::iterator_range<pz::const_svarint_iterator<int64_t>> ref_iter;
		pz::iterator_range<pz::const_varint_iterator<int32_t>> type_iter;

		while (relation.next()) {
			switch (relation.tag()) {
			case 1: { // ID
				auto id = relation.get_int64();
				FlatVector::GetDataMutable<uint8_t>(output.data[0])[index] = 2;
				FlatVector::GetDataMutable<int64_t>(output.data[1])[index] = id;
				FlatVector::SetNull(output.data[4], index, true);
				FlatVector::SetNull(output.data[5], index, true);
			} break;
			case 2: { // Tag Keys
				key_iter = relation.get_packed_uint32();
			} break;
			case 3: { // Tag Vals
				val_iter = relation.get_packed_uint32();
			} break;
			case 8: { // Roles
				role_iter = relation.get_packed_int32();
			} break;
			case 9: { // Refs
				ref_iter = relation.get_packed_sint64();
			} break;
			case 10: { // Types
				type_iter = relation.get_packed_int32();
			} break;
			default:
				relation.skip();
			}
		}

		// Read tags
		if (!key_iter.empty() && !val_iter.empty()) {
			auto tag_count = key_iter.size();

			auto total_tags = ListVector::GetListSize(output.data[2]);
			ListVector::Reserve(output.data[2], total_tags + tag_count);
			ListVector::SetListSize(output.data[2], total_tags + tag_count);
			auto &tag_entry = FlatVector::GetDataMutable<list_entry_t>(output.data[2])[index];

			tag_entry.offset = total_tags;
			tag_entry.length = tag_count;

			auto &key_vector = MapVector::GetKeys(output.data[2]);
			auto &value_vector = MapVector::GetValues(output.data[2]);

			auto keys = key_iter.begin();
			auto vals = val_iter.begin();
			for (idx_t i = tag_entry.offset; i < tag_entry.offset + tag_count; i++) {
				FlatVector::GetDataMutable<string_t>(key_vector)[i] =
				    StringVector::AddString(key_vector, string_table[*keys++]);
				FlatVector::GetDataMutable<string_t>(value_vector)[i] =
				    StringVector::AddString(value_vector, string_table[*vals++]);
			}
		} else {
			FlatVector::SetNull(output.data[2], index, true);
		}

		// Roles
		if (!role_iter.empty()) {
			auto role_count = role_iter.size();

			auto total_roles = ListVector::GetListSize(output.data[6]);
			ListVector::Reserve(output.data[6], total_roles + role_count);
			ListVector::SetListSize(output.data[6], total_roles + role_count);
			auto &role_entry = FlatVector::GetDataMutable<list_entry_t>(output.data[6])[index];
			auto &role_vector = ListVector::GetChildMutable(output.data[6]);
			role_entry.offset = total_roles;
			role_entry.length = role_count;

			auto roles = role_iter.begin();
			for (idx_t i = role_entry.offset; i < role_entry.offset + role_count; i++) {
				auto &role_str = string_table[*roles++];
				if (role_str.empty()) {
					FlatVector::SetNull(role_vector, i, true);
				} else {
					FlatVector::GetDataMutable<string_t>(role_vector)[i] = StringVector::AddString(role_vector, role_str);
				}
			}
		} else {
			FlatVector::SetNull(output.data[6], index, true);
		}

		// Refs
		if (!ref_iter.empty()) {
			auto ref_count = ref_iter.size();

			auto total_refs = ListVector::GetListSize(output.data[3]);
			ListVector::Reserve(output.data[3], total_refs + ref_count);
			ListVector::SetListSize(output.data[3], total_refs + ref_count);
			auto &ref_entry = FlatVector::GetDataMutable<list_entry_t>(output.data[3])[index];
			auto &ref_vector = ListVector::GetChildMutable(output.data[3]);
			ref_entry.offset = total_refs;
			ref_entry.length = ref_count;

			auto ref_data = FlatVector::GetDataMutable<int64_t>(ref_vector);

			int64_t last_ref = 0;
			for (auto ref : ref_iter) {
				last_ref += ref;
				ref_data[total_refs++] = last_ref;
			}
		} else {
			FlatVector::SetNull(output.data[3], index, true);
		}

		// Types
		if (!type_iter.empty()) {
			auto type_count = type_iter.size();

			auto total_types = ListVector::GetListSize(output.data[7]);
			ListVector::Reserve(output.data[7], total_types + type_count);
			ListVector::SetListSize(output.data[7], total_types + type_count);
			auto &type_entry = FlatVector::GetDataMutable<list_entry_t>(output.data[7])[index];
			auto &type_vector = ListVector::GetChildMutable(output.data[7]);
			type_entry.offset = total_types;
			type_entry.length = type_count;

			auto type_data = FlatVector::GetDataMutable<uint8_t>(type_vector);
			for (auto type : type_iter) {
				type_data[total_types++] = (uint8_t)type;
			}
		} else {
			FlatVector::SetNull(output.data[7], index, true);
		}

		index++;
	}

	// Returns true if done (all dense nodes have been read)
	bool ScanDenseNodes(DataChunk &output, idx_t &index, idx_t capacity) {
		// Write multiple nodes at once as long as we have capacity
		auto nodes_to_write = capacity - index;
		auto nodes_to_read = std::min(nodes_to_write, dense_node_ids.size() - dense_node_index);

		auto kind_data = FlatVector::GetDataMutable<uint8_t>(output.data[0]);
		auto id_data = FlatVector::GetDataMutable<int64_t>(output.data[1]);
		auto lat_data = FlatVector::GetDataMutable<double>(output.data[4]);
		auto lon_data = FlatVector::GetDataMutable<double>(output.data[5]);

		for (idx_t i = 0; i < nodes_to_read; i++) {
			auto id = dense_node_ids[dense_node_index];

			id_data[index] = id;
			kind_data[index] = 0;
			lat_data[index] = 0.000000001 * (lat_offset + (granularity * dense_node_lats[dense_node_index]));
			lon_data[index] = 0.000000001 * (lon_offset + (granularity * dense_node_lons[dense_node_index]));

			// Do we have tags in this block?
			if (!dense_node_tags.empty()) {
				auto entry = dense_node_tag_entries[dense_node_index];
				if (entry.length != 0) {
					// Dense nodes tags are stored as a list of key/value pairs,
					// therefore we need to divide the length by 2 to get the number of tags
					auto tag_count = entry.length / 2;

					auto total_tags = ListVector::GetListSize(output.data[2]);
					ListVector::Reserve(output.data[2], total_tags + tag_count);
					ListVector::SetListSize(output.data[2], total_tags + tag_count);
					auto &tag_entry = FlatVector::GetDataMutable<list_entry_t>(output.data[2])[index];

					tag_entry.offset = total_tags;
					tag_entry.length = tag_count;

					auto &key_vector = MapVector::GetKeys(output.data[2]);
					auto &value_vector = MapVector::GetValues(output.data[2]);

					idx_t t = entry.offset;
					idx_t r = tag_entry.offset;
					for (idx_t i = 0; i < tag_count; i++) {

						auto key_id = dense_node_tags[t];
						auto val_id = dense_node_tags[t + 1];

						FlatVector::GetDataMutable<string_t>(key_vector)[r] =
						    StringVector::AddString(key_vector, string_table[key_id]);
						FlatVector::GetDataMutable<string_t>(value_vector)[r] =
						    StringVector::AddString(value_vector, string_table[val_id]);

						t += 2;
						r += 1;
					}
				} else {
					FlatVector::SetNull(output.data[2], index, true);
				}
			} else {
				FlatVector::SetNull(output.data[2], index, true);
			}
			FlatVector::SetNull(output.data[3], index, true);

			// No ref types or roles for dense nodes
			FlatVector::SetNull(output.data[6], index, true);
			FlatVector::SetNull(output.data[7], index, true);

			dense_node_index++;
			index++;
		}
		if (dense_node_index >= dense_node_ids.size()) {
			return true;
		}
		return false;
	}
};

unique_ptr<LocalTableFunctionState> InitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                              GlobalTableFunctionState *global_state) {
	auto &global = global_state->Cast<GlobalState>();
	const auto blob = global.GetNextBlob(context.client);
	if (blob == nullptr) {
		return nullptr;
	}
	auto block = DecompressBlob(context.client, *blob);

	auto result = make_uniq<LocalState>(std::move(block));
	return std::move(result);
}

void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	if (input.local_state == nullptr) {
		return;
	}

	// auto &bind_data = (BindData &)*input.bind_data;
	auto &global_state = input.global_state->Cast<GlobalState>();
	auto &local_state = input.local_state->Cast<LocalState>();

	idx_t row_id = 0;
	idx_t capacity = STANDARD_VECTOR_SIZE;

	while (row_id < capacity) {
		bool done = local_state.TryRead(output, row_id, capacity);
		if (done) {
			auto next = global_state.GetNextBlob(context);
			if (next.get() == nullptr) {
				break;
			}
			auto next_block = DecompressBlob(context, *next);
			local_state.SetBlock(std::move(next_block));
		}
	}
	output.SetCardinality(row_id);
}

double Progress(ClientContext &context, const FunctionData *bind_data, const GlobalTableFunctionState *global_state) {
	const auto &state = global_state->Cast<GlobalState>();
	return state.GetProgress();
}

OperatorPartitionData GetPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("ST_ReadOSM::GetPartitionData: partition columns not supported");
	}
	auto &state = input.local_state->Cast<LocalState>();
	return OperatorPartitionData(state.block->block_idx);
}

unique_ptr<TableRef> ReadOsmPBFReplacementScan(ClientContext &context, ReplacementScanInput &input,
                                               optional_ptr<ReplacementScanData> data) {
	auto &table_name = input.table_name;
	// Check if the table name ends with .osm.pbf
	if (!StringUtil::EndsWith(StringUtil::Lower(table_name), ".osm.pbf")) {
		return nullptr;
	}

	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(Value(table_name)));
	table_function->function = make_uniq<FunctionExpression>("ST_ReadOSM", std::move(children));
	return std::move(table_function);
}

//------------------------------------------------------------------------------
// Documentation
//------------------------------------------------------------------------------

// static constexpr DocTag DOC_TAGS[] = {{"ext", "spatial"}};

constexpr const char *DOC_DESCRIPTION = R"(
    The `ST_ReadOsm()` table function enables reading compressed OpenStreetMap data directly from a `.osm.pbf` file.

    This function uses multithreading and zero-copy protobuf parsing which makes it a lot faster than using the `ST_Read()` OSM driver, however it only outputs the raw OSM data (Nodes, Ways, Relations), without constructing any geometries. For simple node entities (like PoI's) you can trivially construct POINT geometries, but it is also possible to construct LINESTRING and POLYGON geometries by manually joining refs and nodes together in SQL, although with available memory usually being a limiting factor.
    The `ST_ReadOSM()` function also provides a "replacement scan" to enable reading from a file directly as if it were a table. This is just syntax sugar for calling `ST_ReadOSM()` though. Example:

    ```sql
    SELECT * FROM 'tmp/data/germany.osm.pbf' LIMIT 5;
    ```
)";

constexpr const char *DOC_EXAMPLE = R"(
    SELECT *
    FROM ST_ReadOSM('tmp/data/germany.osm.pbf')
    WHERE tags['highway'] != []
    LIMIT 5;
    ----
    ┌──────────────────────┬────────┬──────────────────────┬─────────┬────────────────────┬────────────┬───────────┬────────────────────────┐
    │         kind         │   id   │         tags         │  refs   │        lat         │    lon     │ ref_roles │       ref_types        │
    │ enum('node', 'way'…  │ int64  │ map(varchar, varch…  │ int64[] │       double       │   double   │ varchar[] │ enum('node', 'way', …  │
    ├──────────────────────┼────────┼──────────────────────┼─────────┼────────────────────┼────────────┼───────────┼────────────────────────┤
    │ node                 │ 122351 │ {bicycle=yes, butt…  │         │         53.5492951 │   9.977553 │           │                        │
    │ node                 │ 122397 │ {crossing=no, high…  │         │ 53.520990100000006 │ 10.0156924 │           │                        │
    │ node                 │ 122493 │ {TMC:cid_58:tabcd_…  │         │ 53.129614600000004 │  8.1970173 │           │                        │
    │ node                 │ 123566 │ {highway=traffic_s…  │         │ 54.617268200000005 │  8.9718171 │           │                        │
    │ node                 │ 125801 │ {TMC:cid_58:tabcd_…  │         │ 53.070685000000005 │  8.7819939 │           │                        │
    └──────────────────────┴────────┴──────────────────────┴─────────┴────────────────────┴────────────┴───────────┴────────────────────────┘
)";

} // namespace

//------------------------------------------------------------------------------
//  Register
//------------------------------------------------------------------------------
void RegisterOSMModule(ExtensionLoader &loader) {
	TableFunction read("ST_ReadOSM", {LogicalType::VARCHAR}, Execute, Bind, InitGlobal, InitLocal);

	read.get_partition_data = GetPartitionData;
	read.table_scan_progress = Progress;

	loader.RegisterFunction(read);

	InsertionOrderPreservingMap<string> tags;
	tags.insert("ext", "spatial");
	FunctionBuilder::AddTableFunctionDocs(loader, "ST_ReadOSM", DOC_DESCRIPTION, DOC_EXAMPLE, tags);

	// Replacement scan
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.replacement_scans.emplace_back(ReadOsmPBFReplacementScan);
}

} // namespace duckdb
