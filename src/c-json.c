
#include <assert.h>
#include <ctype.h>
#include <c-stdaux.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include "c-json.h"

struct CJson {
        const char *input;

        /*
         * Current position in the input. Always points to the start of
         * the next value.
         */
        const char *p;

        /*
         * Last error code. If it is non-zero, every function turns into
         * a no-op and returns this value.
         */
        int poison;

        /*
         * State for each nesting level. @n_states is the maximum
         * nesting depth. For each level, the state can be:
         *
         *  0: root level
         *
         *  '[': in an array and @p points to the next value or ']'
         *  ',': in an array and @p is behind a ','
         *
         *  '{': in an object and @p points to the next key
         *  ':': in an object and @p points to the next value
         */
        size_t n_states;
        size_t level;
        char states[];
};

static const char * skip_space(const char *p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                p += 1;

        return p;
}

/*
 * Advances json->p to the start of the next value. Must be called
 * excactly once after a value has been read.
 */
static int c_json_advance(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        json->p = skip_space(json->p);

        switch (json->states[json->level]) {
                case '[':
                        if (*json->p == ',') {
                                json->states[json->level] = ',';
                                json->p = skip_space(json->p + 1);
                        } else if (*json->p != ']')
                                return (json->poison = C_JSON_E_INVALID_JSON);
                        break;

                case ',':
                        if (*json->p == ',')
                                json->p = skip_space(json->p + 1);
                        else if (*json->p == ']')
                                json->states[json->level] = '[';
                        else
                                return (json->poison = C_JSON_E_INVALID_JSON);
                        break;

                case '{':
                        if (*json->p == ':') {
                                json->states[json->level] = ':';
                                json->p = skip_space(json->p + 1);
                        }
                        else
                                return (json->poison = C_JSON_E_INVALID_JSON);
                        break;

                case ':':
                        if (*json->p == ',') {
                                json->states[json->level] = '{';
                                json->p = skip_space(json->p + 1);
                                if (*json->p != '"')
                                        return (json->poison = C_JSON_E_INVALID_JSON);
                        } else if (*json->p != '}')
                                return (json->poison = C_JSON_E_INVALID_JSON);
                        break;
        }

        return 0;
}

/*
 * Reads a single utf-16 code unit and writes it to @unitp. Does not
 * do any unicode validation, as per the spec.
 *
 * Return: 0 on success
 *         C_JSON_E_INVALID_JSON if @p does not point to a valid sequence
 */
static int c_json_read_utf16_unit(const char *p, uint16_t *unitp) {
        uint8_t digits[4];

        for (size_t i = 0; i < 4; i += 1) {
                switch (p[i]) {
                        case '0' ... '9':
                                digits[i] = p[i] - '0';
                                break;

                        case 'a' ... 'f':
                                digits[i] = p[i] - 'a' + 0x0a;
                                break;

                        case 'A' ... 'F':
                                digits[i] = p[i] - 'A' + 0x0a;
                                break;

                        default:
                                return C_JSON_E_INVALID_JSON;
                }
        }

        *unitp = digits[0] << 12 | digits[1] << 8 | digits[2] << 4 | digits[3];

        return 0;
}

static int c_json_write_utf8(uint32_t cp, FILE *stream) {
	switch (cp) {
	case  0x0000 ...   0x007F:
                if (fputc((char)cp, stream) < 0)
			return -ENOTRECOVERABLE;
		break;
	case  0x0080 ...   0x07FF:
                if (fputc((char)(0xc0 | (cp >> 6)), stream) < 0)
			return -ENOTRECOVERABLE;
                if (fputc((char)(0x80 | (cp & 0x3f)), stream) < 0)
			return -ENOTRECOVERABLE;
		break;
	case  0x0800 ...   0xFFFF:
                if (fputc((char)(0xe0 | (cp >> 12)), stream) < 0)
			return -ENOTRECOVERABLE;
                if (fputc((char)(0x80 | ((cp >> 6) & 0x3f)), stream) < 0)
			return -ENOTRECOVERABLE;
                if (fputc((char)(0x80 | (cp & 0x3f)), stream) < 0)
			return -ENOTRECOVERABLE;
		break;
	case 0x10000 ... 0x10FFFF:
                if (fputc((char)(0xf0 | (cp >> 18)), stream) < 0)
			return -ENOTRECOVERABLE;
                if (fputc((char)(0x80 | ((cp >> 12) & 0x3f)), stream) < 0)
			return -ENOTRECOVERABLE;
                if (fputc((char)(0x80 | ((cp >> 6) & 0x3f)), stream) < 0)
			return -ENOTRECOVERABLE;
                if (fputc((char)(0x80 | (cp & 0x3f)), stream) < 0)
			return -ENOTRECOVERABLE;
		break;
	default:
		assert(0);
	}

	return 0;
}

/**
 * c_json_new() - allocate and initialize a CJSon struct
 * @jsonp:              return location
 * @max_depth:          maximum nesting depth
 *
 * Return: <0 on fatal failures
 *         0 on success
 */
_c_public_ int c_json_new(CJson **jsonp, size_t max_depth) {
        _c_cleanup_(c_json_freep) CJson *json = NULL;

        json = calloc(1, sizeof(*json) + max_depth + 1);
        if (!json)
                return -ENOMEM;

        json->n_states = max_depth;

        *jsonp = json;
        json = NULL;

        return 0;
}

/**
 * c_json_free() - deinitialize and free a CJSon struct
 * @json:               json to free
 *
 * Return: NULL
 */
_c_public_ CJson * c_json_free(CJson *json) {
        free(json);

        return NULL;
}

/**
 * c_json_peek - peek at the next value
 * @json                json object
 *
 * Peeks at the next value in the input stream. Does not validate that
 * the next value is valid.
 *
 * Return: -1 if the next token is invalid or when exiting a container
 *         one of the C_JSON_TYPE_ values
 */
_c_public_ int c_json_peek(CJson *json) {
        if (_c_unlikely_(json->poison))
                return -1;

        switch (*json->p) {
                case '[':
                        return C_JSON_TYPE_ARRAY;

                case '{':
                        return C_JSON_TYPE_OBJECT;

                case '"':
                        return C_JSON_TYPE_STRING;

                case '0' ... '9':
                case '-':
                        return C_JSON_TYPE_NUMBER;

                case 't':
                case 'f':
                        return C_JSON_TYPE_BOOLEAN;

                case 'n':
                        return C_JSON_TYPE_NULL;

                default:
                case ']':
                case '}':
                        return -1;
        }
}

/**
 * c_json_begin_read() - begin readinh JSON from a string
 * @json                json object
 * @string              0-terminated string to read from
 *
 * It is an error to call this function multiple times without calling
 * c_json_end_read().
 */
_c_public_ void c_json_begin_read(CJson *json, const char *string) {
        assert(!json->input);

        json->input = string;
        json->p = skip_space(json->input);
}

/**
 * c_json_end_read() - end reading
 * @json                json object
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if called before the input was fully read
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 */
_c_public_ int c_json_end_read(CJson *json) {
        int r = json->poison;

        if (!r) {
                if (json->level > 0)
                        r = C_JSON_E_INVALID_TYPE;

                if (json->level == 0 && *json->p != '\0')
                        r = C_JSON_E_INVALID_JSON;
        }

        json->level = 0;
        json->input = NULL;
        json->p = NULL;

        return r;
}

/**
 * c_json_read_null() - read `null` value
 * @json                json object
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if then next value is not `null`
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 */
_c_public_ int c_json_read_null(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        if (json->states[json->level] == '{')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        switch (*json->p) {
                case 'n':
                        if (strncmp(json->p, "null", strlen("null")))
                                return (json->poison = C_JSON_E_INVALID_JSON);
                        json->p += strlen("null");
                        break;

                default:
                        return (json->poison = C_JSON_E_INVALID_TYPE);
        }

        return c_json_advance(json);
}

/**
 * c_json_read_string() - read a string
 * @json                json object
 * @srtringp            return location for the string
 *
 * The returned string must be freed.
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if then next value is not a string
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 */
_c_public_ int c_json_read_string(CJson *json, char **stringp) {
        _c_cleanup_(c_fclosep) FILE *stream = NULL;
        _c_cleanup_(c_freep) char *string = NULL;
        size_t size;
        int r;

        if (_c_unlikely_(json->poison))
                return json->poison;

        if (*json->p != '"')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        stream = open_memstream(&string, &size);
        if (!stream)
                return (json->poison = -ENOTRECOVERABLE);

        json->p += 1;
        while (*json->p != '"') {
                if ((uint8_t)*json->p < 0x20)
                        return (json->poison = C_JSON_E_INVALID_JSON);

		if (*json->p == '\\') {
                        json->p += 1;
                        switch (*json->p) {
                                case '"':
					json->p += 1;
                                        if (fputc('"', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case '\\':
					json->p += 1;
                                        if (fputc('\\', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case '/':
					json->p += 1;
                                        if (fputc('/', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'b':
					json->p += 1;
                                        if (fputc('\b', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'f':
					json->p += 1;
                                        if (fputc('\f', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'n':
					json->p += 1;
                                        if (fputc('\n', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'r':
					json->p += 1;
                                        if (fputc('\r', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 't':
					json->p += 1;
                                        if (fputc('\t', stream) < 0)
                                                return (json->poison = -ENOTRECOVERABLE);
                                        break;

                                case 'u': {
					uint16_t cu;
					uint32_t cp;
                                        int r;

					json->p += 1;

                                        r = c_json_read_utf16_unit(json->p, &cu);
                                        if (r)
                                                return (json->poison = r);
                                        json->p += 4;

					switch (cu) {
					case 0xD800 ... 0xDBFF:
						cp = 0x10000 + ((cu - 0xD800) << 10);

						if (json->p[0] != '\\' || json->p[1] != 'u')
							return (json->poison = C_JSON_E_INVALID_JSON);
						json->p +=2;

						r = c_json_read_utf16_unit(json->p, &cu);
						if (r)
							return (json->poison = r);
						json->p += 4;

						if (cu < 0xDC00 || cu > 0xDFFF)
							return (json->poison = C_JSON_E_INVALID_JSON);

						cp += cu - 0xDC00;

						break;
					case 0xDC00 ... 0xDFFF:
						return (json->poison = C_JSON_E_INVALID_JSON);
					default:
						cp = cu;
						break;
					}

					r = c_json_write_utf8(cp, stream);
					if (r)
						return (json->poison = r);

                                        break;
                                }

                                default:
                                        return (json->poison = C_JSON_E_INVALID_JSON);
                        }

                } else {
			if (fputc(*json->p, stream) < 0)
				return (json->poison = -ENOTRECOVERABLE);
			json->p += 1;
		}
        }

        json->p += 1; /* '"' */

        stream = c_fclose(stream);

        r = c_json_advance(json);
        if (r)
                return r;

        if (stringp) {
                *stringp = string;
                string = NULL;
        }

        return 0;
}

/**
 * c_json_read_u64() - read a unsigned integer
 * @json                json object
 * @srtringp            return location for the integer
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if then next value is not an unsigned integer
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 */
_c_public_ int c_json_read_u64(CJson *json, uint64_t *numberp) {
        char *end;
        uint64_t number;
        int r;

        if (_c_unlikely_(json->poison))
                return json->poison;

        if (json->states[json->level] == '{')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        /* strtoul() silently flips sign if first char is a minus */
        if (*json->p == '-')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        number = strtoul(json->p, &end, 10);

        if (end == json->p || *end == '.' || *end == 'e' || *end == 'E')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        json->p = end;

        r = c_json_advance(json);
        if (r)
                return r;

        if (numberp)
                *numberp = number;

        return 0;
}

/**
 * c_json_read_f64() - read a number
 * @json                json object
 * @srtringp            return location for the number
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if then next value is not a number
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 */
_c_public_ int c_json_read_f64(CJson *json, double *numberp) {
        char *end;
        double number;
        locale_t loc;
        int r;

        if (_c_unlikely_(json->poison))
                return json->poison;

        if (json->states[json->level] == '{')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t) 0);
        uselocale(loc);
        number = strtod(json->p, &end);
        freelocale(loc);

        if (end == json->p)
                return (json->poison = C_JSON_E_INVALID_JSON);

        json->p = end;

        r = c_json_advance(json);
        if (r)
                return r;

        if (numberp)
                *numberp = number;

        return 0;
}

/**
 * c_json_read_bool() - read a boolean
 * @json                json object
 * @srtringp            return location for the boolean
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if then next value is not a boolean
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 */
_c_public_ int c_json_read_bool(CJson *json, bool *boolp) {
        bool b;
        int r;

        if (json->states[json->level] == '{')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        if (_c_unlikely_(json->poison))
                return json->poison;

        switch (*json->p) {
                case 't':
                        if (strncmp(json->p, "true", strlen("true")))
                                return (json->poison = C_JSON_E_INVALID_JSON);
                        b = true;
                        json->p += strlen("true");
                        break;

                case 'f':
                        if (strncmp(json->p, "false", strlen("false")))
                                return (json->poison = C_JSON_E_INVALID_JSON);
                        b = false;
                        json->p += strlen("false");
                        break;

                default:
                        return (json->poison = C_JSON_E_INVALID_TYPE);
        }

        r = c_json_advance(json);
        if (r)
                return r;

        if (boolp)
                *boolp = b;

        return 0;
}

/**
 * c_json_more() - is another value available
 * @json                json object
 *
 * Return: true if another value is available in the current container
 *         false if no value is available or an error occured in a call to a previous function
 */
_c_public_ bool c_json_more(CJson *json) {
        if (_c_unlikely_(json->poison))
                return false;

        if (!*json->p)
                return false;

        switch (json->states[json->level]) {
                case '[':
                        return *json->p != ']';

                case '{':
                case ':':
                        return *json->p != '}';
        }

        return true;
}

/**
 * c_json_open_array() - enter into an array
 * @json                json object
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if then next value is not an array
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 *         C_JSON_E_DEPTH_OVERFLOW if the nesting depth is too high
 */
_c_public_ int c_json_open_array(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        if (json->states[json->level] == '{')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        if (*json->p != '[')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        if (json->level >= json->n_states)
                return (json->poison = C_JSON_E_DEPTH_OVERFLOW);

        json->p = skip_space(json->p + 1);
        json->states[++json->level] = '[';

        return 0;
}

/**
 * c_json_close_array() - exit from an array
 * @json                json object
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if not currently in an array
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 */
_c_public_ int c_json_close_array(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        if (json->states[json->level] != '[' && json->states[json->level] != ',')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        if (*json->p != ']')
                return (json->poison = C_JSON_E_INVALID_JSON);

        json->p += 1;
        json->level -= 1;

        return c_json_advance(json);
}

/**
 * c_json_open_object() - enter into an object
 * @json                json object
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if then next value is not an array
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 *         C_JSON_E_DEPTH_OVERFLOW if the nesting depth is too high
 */
_c_public_ int c_json_open_object(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        if (json->states[json->level] == '{')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        if (*json->p != '{')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        if (json->level >= json->n_states)
                return (json->poison = C_JSON_E_DEPTH_OVERFLOW);

        json->p = skip_space(json->p + 1);
        if (*json->p != '"' && *json->p != '}')
                return (json->poison = C_JSON_E_INVALID_JSON);

        json->states[++json->level] = '{';

        return 0;
}

/**
 * c_json_close_object() - exit from an object
 * @json                json object
 *
 * Return: <0 on fatal error
 *         0 on success
 *         the last error that occured in a reader function
 *         C_JSON_E_INVALID_TYPE if not currently in an object
 *         C_JSON_E_INVALID_JSON if the JSON input is malformed
 */
_c_public_ int c_json_close_object(CJson *json) {
        if (_c_unlikely_(json->poison))
                return json->poison;

        if (json->states[json->level] != '{' && json->states[json->level] != ':')
                return (json->poison = C_JSON_E_INVALID_TYPE);

        if (*json->p != '}')
                return (json->poison = C_JSON_E_INVALID_JSON);

        json->p += 1;
        json->level -= 1;

        return c_json_advance(json);
}
