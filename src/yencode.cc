#include <node_api.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

#include "encoder.h"
#include "decoder.h"
#include "crc.h"

using namespace RapidYenc;

// TODO: encode should return col num for incremental processing
//       line limit + return input consumed
//       async processing?

static inline size_t YENC_MAX_SIZE(size_t len, size_t line_size) {
	size_t ret = len * 2    /* all characters escaped */
		+ 2 /* allocation for offset and that a newline may occur early */
#if !defined(YENC_DISABLE_AVX256)
		+ 64 /* allocation for YMM overflowing */
#else
		+ 32 /* allocation for XMM overflowing */
#endif
		;
	/* add newlines, considering the possibility of all chars escaped */
	if (line_size == 128) // optimize common case
		return ret + 2 * (len >> 6);
	return ret + 2 * ((len * 2) / line_size);
}

static void free_buffer(napi_env, void* data, void*) {
	free(data);
}

static void init_all() {
	static std::once_flag init_flag;
	std::call_once(init_flag, []() {
		encoder_init();
		decoder_init();
		crc32_init();
	});
}

static napi_value ThrowTypeError(napi_env env, const char* msg) {
	napi_throw_type_error(env, nullptr, msg);
	return nullptr;
}

static napi_value ThrowRangeError(napi_env env, const char* msg) {
	napi_throw_range_error(env, nullptr, msg);
	return nullptr;
}

static napi_value ThrowError(napi_env env, const char* msg) {
	napi_throw_error(env, nullptr, msg);
	return nullptr;
}

static napi_value Undefined(napi_env env) {
	napi_value value;
	napi_get_undefined(env, &value);
	return value;
}

static bool GetBufferInfo(napi_env env, napi_value value, const uint8_t** data, size_t* length) {
	bool is_buffer = false;
	if (napi_is_buffer(env, value, &is_buffer) != napi_ok || !is_buffer)
		return false;
	void* ptr = nullptr;
	if (napi_get_buffer_info(env, value, &ptr, length) != napi_ok)
		return false;
	*data = static_cast<const uint8_t*>(ptr);
	return true;
}

static bool GetMutableBufferInfo(napi_env env, napi_value value, unsigned char** data, size_t* length) {
	bool is_buffer = false;
	if (napi_is_buffer(env, value, &is_buffer) != napi_ok || !is_buffer)
		return false;
	void* ptr = nullptr;
	if (napi_get_buffer_info(env, value, &ptr, length) != napi_ok)
		return false;
	*data = static_cast<unsigned char*>(ptr);
	return true;
}

static bool GetInt32(napi_env env, napi_value value, int32_t* out) {
	return napi_get_value_int32(env, value, out) == napi_ok;
}

static bool GetBool(napi_env env, napi_value value, bool* out) {
	return napi_get_value_bool(env, value, out) == napi_ok;
}

static bool SetNamedProperty(napi_env env, napi_value object, const char* name, napi_value value) {
	return napi_set_named_property(env, object, name, value) == napi_ok;
}

static bool CreateNumber(napi_env env, double value, napi_value* out) {
	return napi_create_double(env, value, out) == napi_ok;
}

static bool CreateInt32(napi_env env, int32_t value, napi_value* out) {
	return napi_create_int32(env, value, out) == napi_ok;
}

static napi_value CreateBuffer(napi_env env, size_t length, unsigned char** data) {
	napi_value buffer;
	void* ptr = nullptr;
	if (napi_create_buffer(env, length, &ptr, &buffer) != napi_ok)
		return ThrowError(env, "Failed to allocate buffer");
	if (data)
		*data = static_cast<unsigned char*>(ptr);
	return buffer;
}

static napi_value CreateExternalBuffer(napi_env env, unsigned char* data, size_t length) {
	napi_value buffer;
	if (napi_create_external_buffer(env, length, data, free_buffer, nullptr, &buffer) != napi_ok) {
		free(data);
		return ThrowError(env, "Failed to create external buffer");
	}
	return buffer;
}

static napi_value PackCrc32(napi_env env, uint32_t crc) {
	unsigned char* data = nullptr;
	napi_value buffer = CreateBuffer(env, 4, &data);
	if (buffer == nullptr)
		return nullptr;
	data[0] = static_cast<unsigned char>((crc >> 24) & 0xFF);
	data[1] = static_cast<unsigned char>((crc >> 16) & 0xFF);
	data[2] = static_cast<unsigned char>((crc >> 8) & 0xFF);
	data[3] = static_cast<unsigned char>(crc & 0xFF);
	return buffer;
}

static inline uint32_t ReadCrc32(const uint8_t* arr) {
	return (static_cast<uint_fast32_t>(arr[0]) << 24)
		| (static_cast<uint_fast32_t>(arr[1]) << 16)
		| (static_cast<uint_fast32_t>(arr[2]) << 8)
		| static_cast<uint_fast32_t>(arr[3]);
}

static napi_value Encode(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 3;
	napi_value args[3];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc == 0)
		return ThrowTypeError(env, "You must supply a Buffer");

	const uint8_t* input = nullptr;
	size_t input_len = 0;
	if (!GetBufferInfo(env, args[0], &input, &input_len))
		return ThrowTypeError(env, "You must supply a Buffer");
	if (input_len == 0)
		return CreateBuffer(env, 0, nullptr);

	int line_size = 128;
	int col = 0;
	if (argc >= 2) {
		int32_t parsed = 0;
		if (!GetInt32(env, args[1], &parsed))
			return ThrowTypeError(env, "Line size must be a number");
		line_size = static_cast<int>(parsed);
		if (line_size == 0)
			line_size = 128;
		if (line_size < 0)
			return ThrowRangeError(env, "Line size must be at least 1 byte");
		if (argc >= 3) {
			if (!GetInt32(env, args[2], &parsed))
				return ThrowTypeError(env, "Column offset must be a number");
			col = static_cast<int>(parsed);
			if (col > line_size || col < 0)
				return ThrowRangeError(env, "Column offset cannot exceed the line size and cannot be negative");
			if (col == line_size)
				col = 0;
		}
	}

	size_t dest_len = YENC_MAX_SIZE(input_len, static_cast<size_t>(line_size));
	unsigned char* result = static_cast<unsigned char*>(malloc(dest_len));
	if (!result)
		return ThrowError(env, "Out of memory");

	size_t len = encode(line_size, &col, input, result, input_len, true);
	unsigned char* shrunk = static_cast<unsigned char*>(realloc(result, len));
	if (shrunk != nullptr)
		result = shrunk;
	else if (len != 0) {
		unsigned char* exact = static_cast<unsigned char*>(malloc(len));
		if (!exact) {
			free(result);
			return ThrowError(env, "Out of memory");
		}
		memcpy(exact, result, len);
		free(result);
		result = exact;
	}

	return CreateExternalBuffer(env, result, len);
}

static napi_value EncodeTo(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 4;
	napi_value args[4];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc < 2)
		return ThrowTypeError(env, "You must supply two Buffers");

	const uint8_t* input = nullptr;
	const uint8_t* output = nullptr;
	size_t input_len = 0;
	size_t output_len = 0;
	if (!GetBufferInfo(env, args[0], &input, &input_len) || !GetBufferInfo(env, args[1], &output, &output_len))
		return ThrowTypeError(env, "You must supply two Buffers");
	if (input_len == 0) {
		napi_value zero;
		if (!CreateNumber(env, 0, &zero))
			return ThrowError(env, "Failed to create return value");
		return zero;
	}

	int line_size = 128;
	int col = 0;
	if (argc >= 3) {
		int32_t parsed = 0;
		if (!GetInt32(env, args[2], &parsed))
			return ThrowTypeError(env, "Line size must be a number");
		line_size = static_cast<int>(parsed);
		if (line_size == 0)
			line_size = 128;
		if (line_size < 0)
			return ThrowRangeError(env, "Line size must be at least 1 byte");
		if (argc >= 4) {
			if (!GetInt32(env, args[3], &parsed))
				return ThrowTypeError(env, "Column offset must be a number");
			col = static_cast<int>(parsed);
			if (col > line_size || col < 0)
				return ThrowRangeError(env, "Column offset cannot exceed the line size and cannot be negative");
			if (col == line_size)
				col = 0;
		}
	}

	size_t dest_len = YENC_MAX_SIZE(input_len, static_cast<size_t>(line_size));
	if (output_len < dest_len)
		return ThrowError(env, "Destination buffer does not have enough space (use `maxSize` to compute required space)");

	size_t len = encode(line_size, &col, input, const_cast<unsigned char*>(output), input_len, true);
	napi_value result;
	if (!CreateNumber(env, static_cast<double>(len), &result))
		return ThrowError(env, "Failed to create return value");
	return result;
}

static napi_value EncodeIncr(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 4;
	napi_value args[4];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc == 0)
		return ThrowTypeError(env, "You must supply a Buffer");

	const uint8_t* input = nullptr;
	size_t input_len = 0;
	if (!GetBufferInfo(env, args[0], &input, &input_len))
		return ThrowTypeError(env, "You must supply a Buffer");

	int line_size = 128;
	int col = 0;
	int argp = 1;
	unsigned char* result = nullptr;
	size_t output_len = 0;
	bool alloc_result = true;
	if (argc > 1) {
		bool is_buffer = false;
		if (napi_is_buffer(env, args[1], &is_buffer) == napi_ok && is_buffer) {
			if (!GetMutableBufferInfo(env, args[1], &result, &output_len))
				return ThrowError(env, "Failed to read destination buffer");
			alloc_result = false;
			argp = 2;
		}
		if (argc > static_cast<size_t>(argp)) {
			int32_t parsed = 0;
			if (!GetInt32(env, args[argp], &parsed))
				return ThrowTypeError(env, "Line size must be a number");
			line_size = static_cast<int>(parsed);
			if (line_size == 0)
				line_size = 128;
			if (line_size < 0)
				return ThrowRangeError(env, "Line size must be at least 1 byte");
			++argp;
			if (argc > static_cast<size_t>(argp)) {
				if (!GetInt32(env, args[argp], &parsed))
					return ThrowTypeError(env, "Column offset must be a number");
				col = static_cast<int>(parsed);
				if (col > line_size || col < 0)
					return ThrowRangeError(env, "Column offset cannot exceed the line size and cannot be negative");
				if (col == line_size)
					col = 0;
			}
		}
	}

	napi_value ret;
	if (napi_create_object(env, &ret) != napi_ok)
		return ThrowError(env, "Failed to create return value");

	if (input_len == 0) {
		napi_value written;
		napi_value col_value;
		if (!CreateNumber(env, 0, &written) || !CreateNumber(env, static_cast<double>(col), &col_value))
			return ThrowError(env, "Failed to create return value");
		if (!SetNamedProperty(env, ret, "written", written) || !SetNamedProperty(env, ret, "col", col_value))
			return ThrowError(env, "Failed to set return value");
		return ret;
	}

	size_t dest_len = YENC_MAX_SIZE(input_len, static_cast<size_t>(line_size));
	if (alloc_result) {
		result = static_cast<unsigned char*>(malloc(dest_len));
		if (!result)
			return ThrowError(env, "Out of memory");
	} else if (output_len < dest_len) {
		return ThrowError(env, "Destination buffer does not have enough space (use `maxSize` to compute required space)");
	}

	size_t len = encode(line_size, &col, input, result, input_len, false);
	napi_value written;
	napi_value col_value;
	if (!CreateNumber(env, static_cast<double>(len), &written) || !CreateNumber(env, static_cast<double>(col), &col_value))
		return ThrowError(env, "Failed to create return value");
	if (!SetNamedProperty(env, ret, "written", written) || !SetNamedProperty(env, ret, "col", col_value))
		return ThrowError(env, "Failed to set return value");

	if (alloc_result) {
		unsigned char* shrunk = static_cast<unsigned char*>(realloc(result, len));
		if (shrunk != nullptr)
			result = shrunk;
		else if (len != 0) {
			unsigned char* exact = static_cast<unsigned char*>(malloc(len));
			if (!exact) {
				free(result);
				return ThrowError(env, "Out of memory");
			}
			memcpy(exact, result, len);
			free(result);
			result = exact;
		}
		napi_value output = CreateExternalBuffer(env, result, len);
		if (output == nullptr)
			return nullptr;
		if (!SetNamedProperty(env, ret, "output", output))
			return ThrowError(env, "Failed to set return value");
	}

	return ret;
}

static napi_value Decode(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 2;
	napi_value args[2];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc == 0)
		return ThrowTypeError(env, "You must supply a Buffer");

	const uint8_t* input = nullptr;
	size_t input_len = 0;
	if (!GetBufferInfo(env, args[0], &input, &input_len))
		return ThrowTypeError(env, "You must supply a Buffer");
	if (input_len == 0)
		return CreateBuffer(env, 0, nullptr);

	bool isRaw = false;
	if (argc > 1) {
		if (!GetBool(env, args[1], &isRaw))
			return ThrowTypeError(env, "stripDots must be a boolean");
	}

	unsigned char* result = static_cast<unsigned char*>(malloc(input_len));
	if (!result)
		return ThrowError(env, "Out of memory");
	size_t len = decode(isRaw, input, result, input_len, nullptr);
	unsigned char* shrunk = static_cast<unsigned char*>(realloc(result, len));
	if (shrunk != nullptr)
		result = shrunk;
	else if (len != 0) {
		unsigned char* exact = static_cast<unsigned char*>(malloc(len));
		if (!exact) {
			free(result);
			return ThrowError(env, "Out of memory");
		}
		memcpy(exact, result, len);
		free(result);
		result = exact;
	}

	return CreateExternalBuffer(env, result, len);
}

static napi_value DecodeTo(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 3;
	napi_value args[3];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc < 2)
		return ThrowTypeError(env, "You must supply two Buffers");

	const uint8_t* input = nullptr;
	unsigned char* output = nullptr;
	size_t input_len = 0;
	size_t output_len = 0;
	if (!GetBufferInfo(env, args[0], &input, &input_len) || !GetMutableBufferInfo(env, args[1], &output, &output_len))
		return ThrowTypeError(env, "You must supply two Buffers");
	if (input_len == 0 || output_len < input_len) {
		napi_value zero;
		if (!CreateNumber(env, 0, &zero))
			return ThrowError(env, "Failed to create return value");
		return zero;
	}

	bool isRaw = false;
	if (argc > 2) {
		if (!GetBool(env, args[2], &isRaw))
			return ThrowTypeError(env, "stripDots must be a boolean");
	}

	size_t len = decode(isRaw, input, output, input_len, nullptr);
	napi_value result;
	if (!CreateNumber(env, static_cast<double>(len), &result))
		return ThrowError(env, "Failed to create return value");
	return result;
}

static napi_value DecodeIncr(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 3;
	napi_value args[3];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc == 0)
		return ThrowTypeError(env, "You must supply a Buffer");

	const uint8_t* input = nullptr;
	size_t input_len = 0;
	if (!GetBufferInfo(env, args[0], &input, &input_len))
		return ThrowTypeError(env, "You must supply a Buffer");
	if (input_len == 0)
		return Undefined(env);

	YencDecoderState state = YDEC_STATE_CRLF;
	unsigned char* result = nullptr;
	bool alloc_result = true;
	if (argc > 1) {
		int32_t parsed = 0;
		if (!GetInt32(env, args[1], &parsed))
			return ThrowTypeError(env, "state must be a number");
		state = static_cast<YencDecoderState>(parsed);
		if (argc > 2) {
			bool is_buffer = false;
			if (napi_is_buffer(env, args[2], &is_buffer) == napi_ok && is_buffer) {
				size_t output_len = 0;
				if (!GetMutableBufferInfo(env, args[2], &result, &output_len))
					return ThrowError(env, "Failed to read destination buffer");
				if (output_len < input_len)
					return ThrowError(env, "Destination buffer does not have enough space");
				alloc_result = false;
			}
		}
	}

	const unsigned char* src = input;
	const unsigned char* sp = src;
	if (alloc_result) {
		result = static_cast<unsigned char*>(malloc(input_len));
		if (!result)
			return ThrowError(env, "Out of memory");
	}
	unsigned char* dp = result;
	YencDecoderEnd ended = RapidYenc::decode_end(reinterpret_cast<const void**>(&sp), reinterpret_cast<void**>(&dp), input_len, &state);
	size_t len = static_cast<size_t>(dp - result);

	napi_value ret;
	if (napi_create_object(env, &ret) != napi_ok)
		return ThrowError(env, "Failed to create return value");

	napi_value read_value;
	napi_value written_value;
	napi_value ended_value;
	napi_value state_value;
	if (!CreateNumber(env, static_cast<double>(sp - src), &read_value)
		|| !CreateNumber(env, static_cast<double>(len), &written_value)
		|| !CreateInt32(env, static_cast<int32_t>(ended), &ended_value)
		|| !CreateInt32(env, static_cast<int32_t>(state), &state_value))
		return ThrowError(env, "Failed to create return value");
	if (!SetNamedProperty(env, ret, "read", read_value)
		|| !SetNamedProperty(env, ret, "written", written_value)
		|| !SetNamedProperty(env, ret, "ended", ended_value)
		|| !SetNamedProperty(env, ret, "state", state_value))
		return ThrowError(env, "Failed to set return value");

	if (alloc_result) {
		unsigned char* shrunk = static_cast<unsigned char*>(realloc(result, len));
		if (shrunk != nullptr)
			result = shrunk;
		else if (len != 0) {
			unsigned char* exact = static_cast<unsigned char*>(malloc(len));
			if (!exact) {
				free(result);
				return ThrowError(env, "Out of memory");
			}
			memcpy(exact, result, len);
			free(result);
			result = exact;
		}
		napi_value output = CreateExternalBuffer(env, result, len);
		if (output == nullptr)
			return nullptr;
		if (!SetNamedProperty(env, ret, "output", output))
			return ThrowError(env, "Failed to set return value");
	}

	return ret;
}

static napi_value CRC32(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 2;
	napi_value args[2];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc == 0)
		return ThrowTypeError(env, "You must supply a Buffer");

	const uint8_t* input = nullptr;
	size_t input_len = 0;
	if (!GetBufferInfo(env, args[0], &input, &input_len))
		return ThrowTypeError(env, "You must supply a Buffer");

	uint32_t crc = 0;
	if (argc >= 2) {
		const uint8_t* initial = nullptr;
		size_t initial_len = 0;
		if (!GetBufferInfo(env, args[1], &initial, &initial_len) || initial_len != 4)
			return ThrowTypeError(env, "Second argument must be a 4 byte buffer");
		crc = ReadCrc32(initial);
	}
	crc = RapidYenc::crc32(input, input_len, crc);
	return PackCrc32(env, crc);
}

static napi_value CRC32Combine(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 3;
	napi_value args[3];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc < 3)
		return ThrowTypeError(env, "At least 3 arguments required");

	const uint8_t* crc1_buf = nullptr;
	const uint8_t* crc2_buf = nullptr;
	size_t crc1_len = 0;
	size_t crc2_len = 0;
	if (!GetBufferInfo(env, args[0], &crc1_buf, &crc1_len) || !GetBufferInfo(env, args[1], &crc2_buf, &crc2_len)
		|| crc1_len != 4 || crc2_len != 4)
		return ThrowTypeError(env, "You must supply a 4 byte Buffer for the first two arguments");

	int32_t len32 = 0;
	if (!GetInt32(env, args[2], &len32))
		return ThrowTypeError(env, "Length must be a number");

	uint32_t crc1 = ReadCrc32(crc1_buf);
	uint32_t crc2 = ReadCrc32(crc2_buf);
	crc1 = crc32_combine(crc1, crc2, static_cast<uint64_t>(len32));
	return PackCrc32(env, crc1);
}

static napi_value CRC32Zeroes(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 2;
	napi_value args[2];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc < 1)
		return ThrowTypeError(env, "At least 1 argument required");

	int32_t len = 0;
	if (!GetInt32(env, args[0], &len))
		return ThrowTypeError(env, "Length must be a number");

	uint32_t crc1 = 0;
	if (argc >= 2) {
		const uint8_t* initial = nullptr;
		size_t initial_len = 0;
		if (!GetBufferInfo(env, args[1], &initial, &initial_len) || initial_len != 4)
			return ThrowTypeError(env, "Second argument must be a 4 byte buffer");
		crc1 = ReadCrc32(initial);
	}
	if (len < 0)
		crc1 = crc32_unzero(crc1, static_cast<uint64_t>(-len));
	else
		crc1 = crc32_zeros(crc1, static_cast<uint64_t>(len));
	return PackCrc32(env, crc1);
}

static napi_value CRC32Multiply(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 2;
	napi_value args[2];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc < 2)
		return ThrowTypeError(env, "At least 2 arguments required");

	const uint8_t* crc1_buf = nullptr;
	const uint8_t* crc2_buf = nullptr;
	size_t crc1_len = 0;
	size_t crc2_len = 0;
	if (!GetBufferInfo(env, args[0], &crc1_buf, &crc1_len) || !GetBufferInfo(env, args[1], &crc2_buf, &crc2_len)
		|| crc1_len != 4 || crc2_len != 4)
		return ThrowTypeError(env, "You must supply a 4 byte Buffer for the first two arguments");

	uint32_t crc1 = ReadCrc32(crc1_buf);
	uint32_t crc2 = ReadCrc32(crc2_buf);
	crc1 = crc32_multiply(crc1, crc2);
	return PackCrc32(env, crc1);
}

static napi_value CRC32Shift(napi_env env, napi_callback_info info) {
	init_all();

	size_t argc = 2;
	napi_value args[2];
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok)
		return ThrowError(env, "Failed to read arguments");
	if (argc < 1)
		return ThrowTypeError(env, "At least 1 argument required");

	int32_t n = 0;
	if (!GetInt32(env, args[0], &n))
		return ThrowTypeError(env, "Length must be a number");

	uint32_t crc1 = 0x80000000;
	if (argc >= 2) {
		const uint8_t* initial = nullptr;
		size_t initial_len = 0;
		if (!GetBufferInfo(env, args[1], &initial, &initial_len) || initial_len != 4)
			return ThrowTypeError(env, "Second argument must be a 4 byte buffer");
		crc1 = ReadCrc32(initial);
	}
	if (n < 0)
		crc1 = crc32_shift(crc1, ~crc32_powmod(static_cast<uint64_t>(-n)));
	else
		crc1 = crc32_shift(crc1, crc32_powmod(static_cast<uint64_t>(n)));
	return PackCrc32(env, crc1);
}

NAPI_MODULE_INIT() {
	init_all();
	static napi_property_descriptor descriptors[] = {
		{"encode", nullptr, Encode, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"decode", nullptr, Decode, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"encodeTo", nullptr, EncodeTo, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"encodeIncr", nullptr, EncodeIncr, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"decodeTo", nullptr, DecodeTo, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"decodeIncr", nullptr, DecodeIncr, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"crc32", nullptr, CRC32, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"crc32_combine", nullptr, CRC32Combine, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"crc32_zeroes", nullptr, CRC32Zeroes, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"crc32_multiply", nullptr, CRC32Multiply, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"crc32_shift", nullptr, CRC32Shift, nullptr, nullptr, nullptr, napi_default, nullptr},
	};
	if (napi_define_properties(env, exports, sizeof(descriptors) / sizeof(descriptors[0]), descriptors) != napi_ok)
		return nullptr;
	return exports;
}
