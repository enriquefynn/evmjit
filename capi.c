/// EVMJIT C Interface
///
/// ## High level design rules
/// 1. Pass function arguments and results by value.
///    This rule comes from modern C++ and tries to avoid costly alias analysis
///    needed for optimization. As the result we have a lots of complex structs
///    and unions. And variable sized arrays of bytes cannot be passed by copy.
/// 2. The EVM operates on integers so it prefers values to be host-endian.
///    On the other hand, LLVM can generate good code for byte swaping.
///    The interface also tries to match host application "natural" endianess.
///    I would like to know what endianess you use and where.
///
/// @defgroup EVMJITCAPI
/// @{

#include <stdint.h>  // Definition of uint64_t.
#include <stddef.h>  // Definition of size_t.

/// Host-endian 256-bit integer.
///
/// 32 bytes of data representing host-endian (that means little-endian almost
/// all the time) 256-bit integer. This applies to the words[] order as well.
/// words[0] contains the 64 lowest precision bits, words[3] constains the 64
/// highest precision bits.
struct evmjit_uint256 {
    uint64_t words[4];
};

/// 160-bit hash suitable for keeping an Ethereum address.
struct evmjit_hash160 {
    char bytes[20];
};


/// Big-endian 256-bit integer/hash.
///
/// 32 bytes of data. For EVM that means big-endian 256-bit integer. Values of
/// this type are converted to host-endian values in EVMJIT.
struct evmjit_hash256 {
    _Alignas(8) char bytes[32];
};

/// Reference to non-mutable memory.
struct evmjit_bytes_view {
    char const* bytes;
    size_t size;
};

/// Reference to mutable memory.
struct evmjit_mutable_bytes_view {
    char* bytes;
    size_t size;
};

enum evmjit_return_code {
    evmjit_return = 0,
    evmjit_selfdestruct = 1,
    evmjit_exception = -1,
};

/// Complex struct representing execution result.
struct evmjit_result {
    /// Success? OOG? Selfdestruction?
    enum evmjit_return_code return_code;
    union {
        /// In case of successful execution this substruct is filled.
        struct {
            /// Rerefence to output data. The memory containing the output data
            /// is owned by EVMJIT and is freed with evmjit_destroy_result().
            struct evmjit_bytes_view output_data;

            /// Gas left after execution. Non-negative.
            /// @todo We could squeeze gas_left and return_code together.
            int64_t gas_left;

            /// Pointer to EVMJIT-owned memory.
            /// @see output_data.
            void* internal_memory;
        };
        /// In case of selfdestruction here is the address where money goes.
        struct evmjit_hash160 selfdestruct_beneficiary;
    };
};

enum evmjit_query_key {
    evmjit_query_gas_price,
    evmjit_query_address,
    evmjit_query_caller,
    evmjit_query_origin,
    evmjit_query_coinbase,
    evmjit_query_difficulty,
    evmjit_query_gas_limit,
    evmjit_query_number,
    evmjit_query_timestamp,
    evmjit_query_code_by_address,
    evmjit_query_balance,
    evmjit_query_storage,
};


/// Opaque struct representing execution enviroment managed by the host
/// application.
struct evmjit_env;

/// Variant type to represent possible types of values used in EVMJIT.
///
/// Type-safety is lost around the code that uses this type. We should have
/// complete set of unit tests covering all possible cases.
/// The size of the type is 64 bytes and should fit in single cache line.
union evmjit_variant {
    uint64_t uint64;
    struct evmjit_uint256 uint256;
    struct {
        char address_padding[12];  // Pad address to lower bytes of full hash.
        struct evmjit_hash160 address;
    };
    struct evmjit_bytes_view bytes;
};

/// Query callback function.
///
/// This callback function is used by the EVMJIT to query the host application
/// about additional data required to execute EVM code.
/// @param env  Pointer to execution environment managed by the host
/// application.
/// @param key  The kind of the query. See evmjit_query_key and defails below.
/// @param arg  Addicional argument to the query. It has defined value only for
///             the subset of query keys.
///
/// # Queries
/// Key             | Arg      | Expected result (see evmjit_query_result union)
/// --------------- | -------- | ---------------
/// gas price       |          | uint256
/// address         |          | address
/// caller          |          | address
/// origin          |          | address
/// coinbase        |          | address
/// difficulty      |          | uint256
/// gas limit       |          | uint64
/// number          |          | uint64?
/// timestamp       |          | uint64?
/// code by address | address  | bytes
/// balance         | address  | uint256
/// storage         | uint256  | uint256?
typedef union evmjit_variant (*evmjit_query_func)(struct evmjit_env* env,
                                                  enum evmjit_query_key key,
                                                  union evmjit_variant arg);


/// Callback function for modifying the storage.
///
/// Endianness: host-endianness is used because C++'s storage API uses big ints,
///             not bytes. What do you use?
typedef void (*evmjit_store_storage_func)(struct evmjit_env*,
                                          struct evmjit_uint256 key,
                                          struct evmjit_uint256 value);

enum evmjit_call_kind {
    evmjit_call,
    evmjit_delegatecall,
    evmjit_callcode,

    /// Request CREATE. Semantic of some params changes.
    evmjit_create
};

/// Pointer to the callback function supporting EVM calls.
///
/// @param kind         The kind of call-like opcode requested.
/// @param gas          The amound of gas for the call.
/// @param address      The address of a contract to be called. Ignored in case
///                     of CREATE.
/// @param value        The value sent to the callee. The endowment in case of
///                     CREATE.
/// @param input_data   The call input data or the create init code.
/// @param output_data  The reference to the memory where the call output is to
///                     be copied. In case of create, the memory is guaranteed
///                     to be at least 160 bytes to hold the address of the
///                     created contract.
/// @return      If non-negative - the amount of gas left,
///              If negative - an exception ocurred during the call/create.
///              There is no need to set 0 address in the output in this case.
typedef int64_t (*evmjit_call_func)(
    enum evmjit_call_kind kind,
    int64_t gas,
    struct evmjit_hash160 address,
    struct evmjit_uint256 value,
    struct evmjit_bytes_view input_data,
    struct evmjit_mutable_bytes_view output_data);

/// Pointer to the callback function supporting EVM logs.
///
/// @param log_data    Reference to memory containing non-indexed log data.
/// @param num_topics  Number of topics added to the log. Valid values 0-4.
/// @param topics      Pointer to an array containing `num_topics` topics.
typedef void (*evmjit_log_func)(struct evmjit_bytes_view log_data,
                                size_t num_topics,
                                struct evmjit_hash256 topics[]);


/// Returns EVMJIT software version.
///
/// @todo Is int a good type? E.g. version 1.2.30 being 10230?
int evmjit_get_version();

/// Opaque type representing a JIT instance.
struct evmjit_instance;

/// Creates JIT instance.
///
/// Creates new JIT instance. Each instance must be destroyed in
/// evmjit_destroy_instance() function.
/// Single instance is thread-safe and can be shared by many threads. The host
/// application can create as many instances as wanted but there are no benefits
/// of this strategy as instances will not share generated code.
///
/// @params Pointers to callback functions.
struct evmjit_instance* evmjit_create_instance(evmjit_query_func,
                                               evmjit_store_storage_func,
                                               evmjit_call_func,
                                               evmjit_log_func);

/// Destroys JIT instance.
void evmjit_destroy_instance(struct evmjit_instance*);


/// Configures a JIT instance.
///
/// Allows modifying options of a JIT instance.
/// Options:
/// - compatibility mode: frontier, homestead, metropolis, ...
/// - code cache behavior: on, off, read-only, ...
/// - optimizations,
///
/// @todo int is probably not the best choice for params type. Would a c-string
///       as char const* be better?
/// @todo Is there a need for evmjit_get_option()?
void evmjit_set_option(struct evmjit_instance*, int key, int value);


/// Generates and executes machine code for given EVM bytecode.
///
/// All the fun is here. This function actually does something useful.
///
/// @param instance    A JIT instance.
/// @param code_hash   A hash of the bytecode, usually Keccak. EVMJIT uses it as
///                    the code identifier. EVMJIT is able to hash the code
///                    itself, but the host application usually has the hash
///                    already.
/// @param code        Reference to the bytecode to be executed.
/// @param gas         Gas for execution. Min 0, max 2^63-1.
/// @param input_data  Reference to the call input data.
/// @param value       Call value.
/// @return            All execution results.
struct evmjit_result evmjit_execute(struct evmjit_instance* instance,
                                    struct evmjit_hash256 code_hash,
                                    struct evmjit_bytes_view code,
                                    int64_t gas,
                                    struct evmjit_bytes_view input_data,
                                    struct evmjit_uint256 value);

/// Destroys execution result.
void evmjit_destroy_result(struct evmjit_result);

/// @}

/// EXAMPLE ////////////////////////////////////////////////////////////////////

struct evmjit_uint256 balance(struct evmjit_env*,
                              struct evmjit_hash160 address);

union evmjit_variant query(struct evmjit_env* env,
                           enum evmjit_query_key key,
                           union evmjit_variant arg) {
    union evmjit_variant result;
    switch (key) {
    case evmjit_query_gas_limit: result.uint64 = 314; break;

    case evmjit_query_balance:
        result.uint256 = balance(env, arg.address);
        break;

    default: result.uint64 = 0; break;
    }
    return result;
}

/// Example how the API is supposed to be used.
void example() {
    struct evmjit_instance* jit = evmjit_create_instance(query, 0, 0, 0);

    char const code[] = "exec()";
    struct evmjit_bytes_view code_view = {code, sizeof(code)};
    struct evmjit_hash256 code_hash = {{1, 2, 3}};
    struct evmjit_bytes_view input = {"Hello World!", 12};
    struct evmjit_uint256 value = {{1, 0, 0, 0}};

    int64_t gas = 200000;
    struct evmjit_result result =
        evmjit_execute(jit, code_hash, code_view, gas, input, value);

    evmjit_destroy_result(result);
    evmjit_destroy_instance(jit);
}