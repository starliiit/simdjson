// This file contains the common code every implementation uses for stage2
// It is intended to be included multiple times and compiled multiple times
// We assume the file in which it is include already includes
// "simdjson/stage2_build_tape.h" (this simplifies amalgation)

namespace stage2 {

#ifdef SIMDJSON_USE_COMPUTED_GOTO
typedef void* ret_address;
#define INIT_ADDRESSES() { &&array_begin, &&array_continue, &&error, &&finish, &&object_begin, &&object_continue }
#define GOTO(address) { goto *(address); }
#define CONTINUE(address) { goto *(address); }
#else
typedef char ret_address;
#define INIT_ADDRESSES() { '[', 'a', 'e', 'f', '{', 'o' };
#define GOTO(address)                 \
  {                                   \
    switch(address) {                 \
      case '[': goto array_begin;     \
      case 'a': goto array_continue;  \
      case 'e': goto error;           \
      case 'f': goto finish;          \
      case '{': goto object_begin;    \
      case 'o': goto object_continue; \
    }                                 \
  }
// For the more constrained pop_scope() situation
#define CONTINUE(address)             \
  {                                   \
    switch(address) {                 \
      case 'a': goto array_continue;  \
      case 'o': goto object_continue; \
      case 'f': goto finish;          \
    }                                 \
  }
#endif

struct unified_machine_addresses {
  ret_address array_begin;
  ret_address array_continue;
  ret_address error;
  ret_address finish;
  ret_address object_begin;
  ret_address object_continue;
};

#undef FAIL_IF
#define FAIL_IF(EXPR) { if (EXPR) { return addresses.error; } }

// This is just so we can call parse_string() from parser.parse_string() without conflict.
WARN_UNUSED really_inline bool
really_parse_string(const uint8_t *buf, size_t len, ParsedJson &pj, uint32_t depth, uint32_t idx) {
  return parse_string(buf, len, pj, depth, idx);
}
WARN_UNUSED really_inline bool
really_parse_number(const uint8_t *const buf, ParsedJson &pj, const uint32_t offset, bool found_minus) {
  return parse_number(buf, pj, offset, found_minus);
}

struct structural_parser {
  const uint8_t* const buf;
  const size_t len;
  ParsedJson &pj;
  uint32_t i; // next structural index
  uint32_t idx; // location of the structural character in the input (buf)
  uint8_t c;    // used to track the (structural) character we are looking at
  uint32_t depth = 0; // could have an arbitrary starting depth

  really_inline structural_parser(const uint8_t *_buf, size_t _len, ParsedJson &_pj, uint32_t _i = 0) : buf{_buf}, len{_len}, pj{_pj}, i{_i} {}

  WARN_UNUSED really_inline int set_error_code(ErrorValues error_code) {
    pj.error_code = error_code;
    return error_code;
  }

  really_inline char advance_char() {
    idx = pj.structural_indexes[i++];
    c = buf[idx];
    return c;
  }

  template<typename F>
  really_inline bool with_space_terminated_copy(const F& f) {
    /**
    * We need to make a copy to make sure that the string is space terminated.
    * This is not about padding the input, which should already padded up
    * to len + SIMDJSON_PADDING. However, we have no control at this stage
    * on how the padding was done. What if the input string was padded with nulls?
    * It is quite common for an input string to have an extra null character (C string).
    * We do not want to allow 9\0 (where \0 is the null character) inside a JSON
    * document, but the string "9\0" by itself is fine. So we make a copy and
    * pad the input with spaces when we know that there is just one input element.
    * This copy is relatively expensive, but it will almost never be called in
    * practice unless you are in the strange scenario where you have many JSON
    * documents made of single atoms.
    */
    char *copy = static_cast<char *>(malloc(len + SIMDJSON_PADDING));
    if (copy == nullptr) {
      return true;
    }
    memcpy(copy, buf, len);
    memset(copy + len, ' ', SIMDJSON_PADDING);
    bool result = f(reinterpret_cast<const uint8_t*>(copy), idx);
    free(copy);
    return result;
  }

  WARN_UNUSED really_inline bool push_start_scope(ret_address continue_state, char type) {
    pj.containing_scope_offset[depth] = pj.get_current_loc();
    pj.ret_address[depth] = continue_state;
    depth++;
    pj.write_tape(0, type);
    return depth >= pj.depth_capacity;
  }

  WARN_UNUSED really_inline bool push_start_scope(ret_address continue_state) {
    return push_start_scope(continue_state, c);
  }

  WARN_UNUSED really_inline bool push_scope(ret_address continue_state) {
    pj.containing_scope_offset[depth] = pj.get_current_loc();
    pj.write_tape(0, c); // Do this as early as possible
    pj.ret_address[depth] = continue_state;
    depth++;
    return depth >= pj.depth_capacity;
  }

  WARN_UNUSED really_inline ret_address pop_scope() {
    // write our tape location to the header scope
    depth--;
    pj.write_tape(pj.containing_scope_offset[depth], c);
    pj.annotate_previous_loc(pj.containing_scope_offset[depth], pj.get_current_loc());
    return pj.ret_address[depth];
  }
  really_inline void pop_root_scope() {
    // write our tape location to the header scope
    // The root scope gets written *at* the previous location.
    depth--;
    pj.annotate_previous_loc(pj.containing_scope_offset[depth], pj.get_current_loc());
    pj.write_tape(pj.containing_scope_offset[depth], 'r');
  }

  WARN_UNUSED really_inline bool parse_string() {
    return !really_parse_string(buf, len, pj, depth, idx);
  }

  WARN_UNUSED really_inline bool parse_number(const uint8_t *copy, uint32_t offset, bool found_minus) {
    return !really_parse_number(copy, pj, offset, found_minus);
  }
  WARN_UNUSED really_inline bool parse_number(bool found_minus) {
    return parse_number(buf, idx, found_minus);
  }

  WARN_UNUSED really_inline bool parse_atom(const uint8_t *copy, uint32_t offset) {
    switch (c) {
      case 't':
        if (!is_valid_true_atom(copy + offset)) { return true; };
        break;
      case 'f':
        if (!is_valid_false_atom(copy + offset)) { return true; }
        break;
      case 'n':
        if (!is_valid_null_atom(copy + offset)) { return true; }
        break;
      default:
        return false;
    }
    pj.write_tape(0, c);
    return false;
  }

  WARN_UNUSED really_inline bool parse_atom() {
    return parse_atom(buf, idx);
  }

  WARN_UNUSED really_inline ret_address parse_value(const unified_machine_addresses &addresses, ret_address continue_state) {
    switch (c) {
    case '"':
      FAIL_IF( parse_string() );
      return continue_state;
    case 't': case 'f': case 'n':
      FAIL_IF( parse_atom() );
      return continue_state;
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
      FAIL_IF( parse_number(false) );
      return continue_state;
    case '-':
      FAIL_IF( parse_number(true) );
      return continue_state;
    case '{':
      FAIL_IF( push_scope(continue_state) );
      return addresses.object_begin;
    case '[':
      FAIL_IF( push_scope(continue_state) );
      return addresses.array_begin;
    default:
      return addresses.error;
    }
  }

  WARN_UNUSED really_inline int finish() {
    // the string might not be NULL terminated.
    if ( i + 1 != pj.n_structural_indexes ) {
      return set_error_code(TAPE_ERROR);
    }
    pop_root_scope();
    if (depth != 0) {
      return set_error_code(TAPE_ERROR);
    }
    if (pj.containing_scope_offset[depth] != 0) {
      return set_error_code(TAPE_ERROR);
    }

    pj.valid = true;
    return set_error_code(SUCCESS);
  }

  WARN_UNUSED really_inline int error() {
    /* We do not need the next line because this is done by pj.init(),
    * pessimistically.
    * pj.is_valid  = false;
    * At this point in the code, we have all the time in the world.
    * Note that we know exactly where we are in the document so we could,
    * without any overhead on the processing code, report a specific
    * location.
    * We could even trigger special code paths to assess what happened
    * carefully,
    * all without any added cost. */
    if (depth >= pj.depth_capacity) {
      return set_error_code(DEPTH_ERROR);
    }
    switch (c) {
    case '"':
      return set_error_code(STRING_ERROR);
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case '-':
      return set_error_code(NUMBER_ERROR);
    case 't':
      return set_error_code(T_ATOM_ERROR);
    case 'n':
      return set_error_code(N_ATOM_ERROR);
    case 'f':
      return set_error_code(F_ATOM_ERROR);
    default:
      return set_error_code(TAPE_ERROR);
    }
  }

  WARN_UNUSED really_inline int start(ret_address finish_state) {
    pj.init(); // sets is_valid to false
    if (len > pj.byte_capacity) {
      return CAPACITY;
    }
    // Advance to the first character as soon as possible
    advance_char();
    // Push the root scope (there is always at least one scope)
    if (push_start_scope(finish_state, 'r')) {
      return DEPTH_ERROR;
    }
    return SUCCESS;
  }
};

// Redefine FAIL_IF to use goto since it'll be used inside the function now
#undef FAIL_IF
#define FAIL_IF(EXPR) { if (EXPR) { goto error; } }

/************
 * The JSON is parsed to a tape, see the accompanying tape.md file
 * for documentation.
 ***********/
WARN_UNUSED  int
unified_machine(const uint8_t *buf, size_t len, ParsedJson &pj) {
  static constexpr unified_machine_addresses addresses = INIT_ADDRESSES();
  structural_parser parser(buf, len, pj);
  int result = parser.start(addresses.finish);
  if (result) { return result; }

  //
  // Read first value
  //
  switch (parser.c) {
  case '{':
    FAIL_IF( parser.push_start_scope(addresses.finish) );
    goto object_begin;
  case '[':
    FAIL_IF( parser.push_start_scope(addresses.finish) );
    goto array_begin;
  case '"':
    FAIL_IF( parser.parse_string() );
    goto finish;
  case 't': case 'f': case 'n':
    FAIL_IF(
      parser.with_space_terminated_copy([&](auto copy, auto idx) {
        return parser.parse_atom(copy, idx);
      })
    );
    goto finish;
  case '0': case '1': case '2': case '3': case '4':
  case '5': case '6': case '7': case '8': case '9':
    FAIL_IF(
      parser.with_space_terminated_copy([&](auto copy, auto idx) {
        return parser.parse_number(copy, idx, false);
      })
    );
    goto finish;
  case '-':
    FAIL_IF(
      parser.with_space_terminated_copy([&](auto copy, auto idx) {
        return parser.parse_number(copy, idx, true);
      })
    );
    goto finish;
  default:
    goto error;
  }

//
// Object parser states
//
object_begin:
  parser.advance_char();
  switch (parser.c) {
  case '"': {
    FAIL_IF( parser.parse_string() );
    goto object_key_state;
  }
  case '}':
    goto scope_end; // could also go to object_continue
  default:
    goto error;
  }

object_key_state:
  FAIL_IF( parser.advance_char() != ':' );

  parser.advance_char();
  GOTO( parser.parse_value(addresses, addresses.object_continue) );

object_continue:
  switch (parser.advance_char()) {
  case ',':
    FAIL_IF( parser.advance_char() != '"' );
    FAIL_IF( parser.parse_string() );
    goto object_key_state;
  case '}':
    goto scope_end;
  default:
    goto error;
  }

scope_end:
  CONTINUE( parser.pop_scope() );

//
// Array parser states
//
array_begin:
  if (parser.advance_char() == ']') {
    goto scope_end; // could also go to array_continue
  }

main_array_switch:
  /* we call update char on all paths in, so we can peek at parser.c on the
   * on paths that can accept a close square brace (post-, and at start) */
  GOTO( parser.parse_value(addresses, addresses.array_continue) );

array_continue:
  switch (parser.advance_char()) {
  case ',':
    parser.advance_char();
    goto main_array_switch;
  case ']':
    goto scope_end;
  default:
    goto error;
  }

finish:
  return parser.finish();

error:
  return parser.error();
}

} // namespace stage2