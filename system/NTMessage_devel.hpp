#pragma once
////////////////////////////////////////////////////////////////////////
// Copyright (c) 2010-2015, University of Washington and Battelle
// Memorial Institute.  All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//     * Redistributions of source code must retain the above
//       copyright notice, this list of conditions and the following
//       disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials
//       provided with the distribution.
//     * Neither the name of the University of Washington, Battelle
//       Memorial Institute, or the names of their contributors may be
//       used to endorse or promote products derived from this
//       software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// UNIVERSITY OF WASHINGTON OR BATTELLE MEMORIAL INSTITUTE BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
// OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
// USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
// DAMAGE.
////////////////////////////////////////////////////////////////////////

// This file is for us to work in while developing the new NTMessage
// functionality, so that we don't have to rebuild all of Grappa every
// time we change something and want to run a test.  Everything here
// will be moved into NTMessage.hpp once it's ready for integration
// with the rest of Grappa.

#include <NTMessage.hpp>
#include <Addressing.hpp>

namespace Grappa {
namespace impl {


//
// Second, more complete version of NTMessage
//

//
// Messages will consist of up to three parts:
// * an NTHeader, containing
//    * destination core, with enough bits for at least 1M cores (20 or more)
//    * address, with enough bits to represent virtual addresses (44 is enough for now)
//    * deserializer function pointer. If we assume "-mcmodel=small" or "medium", where all code is linked in the lower 2GB of address sapce, we need at most 31 bits.
//    * count: number of times handler should be executed; if the capture or payload are non-empty this will also be the number of data items to read from buffer
//    * message size: including size of stored capture
//
// One of these will be followed by <count> copies of these:
// * optional lambda capture storage, for messages whose lambda captures data
// * optional dynamic payload storage, for messages where payload is non-empty. Payload size must be the same when sharing a header
//
// This will allow us to support a number of different message signatures:
//
// Empty lambdas have a minimum sizeof() of 1, so specialize these to avoid that 1-byte-per-message overhead:
// * send_ntmessage( Core destination, []                                 { ; }                  );
// * send_ntmessage( Core destination, []        (       U * u, size_t s) { ; }, U * u, size_t s );
//
// With a capture, the message size should be sizeof(lambda) + optional size of dynamic argument:
// * send_ntmessage( Core destination, [capture]                          { ; }                  );
// * send_ntmessage( Core destination, [capture] (       U * u, size_t s) { ; }, U * u, size_t s );
//
// Messages with an address target should prefetch the destination address.
// Empty lambdas have a minimum sizeof() of 1, so specialize these to avoid that 1-byte-per-message overhead:
// * send_ntmessage( GlobalAddress<T>, []        (T * t) { ; } );
// * send_ntmessage( GlobalAddress<T>, []        (T & t) { ; } );
// * send_ntmessage( GlobalAddress<T>, []        (T * t, U * u, size_t s) { ; }, U * u, size_t s );
// * send_ntmessage( GlobalAddress<T>, []        (T & t, U * u, size_t s) { ; }, U * u, size_t s );
//
// With a capture, the message size should be sizeof(lambda) + optional sizeof(size of dynamic argument):
// * send_ntmessage( GlobalAddress<T>, [capture] (T & t                 ) { ; }                  );
// * send_ntmessage( GlobalAddress<T>, [capture] (T * t                 ) { ; }                  );
// * send_ntmessage( GlobalAddress<T>, [capture] (T & t, U * u, size_t s) { ; }, U * u, size_t s );
// * send_ntmessage( GlobalAddress<T>, [capture] (T * t, U * u, size_t s) { ; }, U * u, size_t s );
//
// Offset:
// * first message: new header, just store address
// * second message: compute difference between this address and first one, and store
// * third, ... message: compute difference between this address and last. if same, combine. otherwise, start new message
//
// Message combining:
// * for each destination, store: (or just for most recent destination)
//   * function pointer
//   * payload size
//   * last argument address
//   * last argument offset

constexpr int NTMESSAGE_WORD_BITS    = 64;
constexpr int NTMESSAGE_ADDRESS_BITS = 44;

struct NTHeader {
  union {
    struct { // TODO: reorder for efficiency?
      uint32_t dest_  : (NTMESSAGE_WORD_BITS - NTMESSAGE_ADDRESS_BITS); //< destination core
      int64_t addr_   : (NTMESSAGE_ADDRESS_BITS); //< first argument address

      // 33 bits remaining
      
      uint32_t fp_    : 31; //< deserializer function pointer

      //
      // TODO: rebalance these after collecting data
      //
      
      uint16_t size_  : 13; // overall message size (capture + payload). May be zero for no-capture, no-payload messages

      // TODO: how many messages can we practically combine?
      uint16_t count_ : 10; // message count: should be

      // TODO: do we need more than 0 or 1 for this? or do we need something more complicated?
      int16_t offset_ : 10; // for messages with arguments of type T, increment pointer by this much each time
    };
    uint64_t raw_[2]; // for size/alignment
  };
  // NTHeader(Core dest, uint16_t size, uint32_t fp): dest_(dest), size_(size), fp_(fp) { }
  // NTHeader(): dest_(-1), size_(0), fp_(0) { }
  // NTHeader( const NTMessageBase& m ): dest_(m.dest_), size_(m.size_), fp_(m.fp_) { }
  // NTHeader( NTMessageBase&& m ): dest_(m.dest_), size_(m.size_), fp_(m.fp_) { }

  // static char * deserialize_and_call( char * t ) {
  //   // TODO: what goes here? Specialize for various cases above
  //   NTMessage<T> * tt = reinterpret_cast< NTMessage<T> * >( t );
  //   tt->storage_();
  //   return t + sizeof( NTMessage<T> );
  // }
  
}  __attribute__((aligned(8))); // TODO: verify alignment?


//
// Messages without payload or address
//

template< typename H, // handler type
          // is this a function pointer or no-capture lambda?
          bool cast_to_voidstarvoid = (std::is_convertible<H,void(*)(void)>::value),
          // if this is a lambda with capture, can we fit it entirely in the address bits?
          bool fits_in_address      = (sizeof(H) * 8 <= NTMESSAGE_ADDRESS_BITS) >
struct NTMessageSpecializer : public NTHeader {
  static void send_ntmessage( Core destination, H handler );
  static char * deserialize_and_call( char * t );
};

// Specializer for no-payload no-capture no-address message
template< typename H, bool dontcare >
struct NTMessageSpecializer<H, true, dontcare> {
  static void send_ntmessage( Core destination, H handler ) {
    LOG(INFO) << "No address; handler has empty capture: " << __PRETTY_FUNCTION__;
    handler();
    // dest_ = destination;
    // addr_ = 0; // don't care; unused
    // offset_ = 0; // don't care; unused
    // fp_ = make_32bit(&deserialize_and_call);
    // size_ = sizeof(*this); // should be 16 bytes, same as header
    // count_ = 1; // increment if previous call was the same
    // // call nt_enqueue on this, or something like that
  }
  // static char * deserialize_and_call( char * buf ) {
  //   auto header_p = reinterpret_cast< decltype(this) >( buf );
  //   for( int i = 0; i < header_p->count_; ++i ) {
  //     H(); // ???
  //   }
  //   return buf + sizeof(*this);
  // }
};

// Specializer for no-payload no-address message with capture that fits in address bits
template< typename H >
struct NTMessageSpecializer<H, false, true > {
  static void send_ntmessage( Core destination, H handler ) {
    LOG(INFO) << "No address; handler of size " << sizeof(H) << " fits in address field: " << __PRETTY_FUNCTION__;
    handler();
  }
};

// Specializer for no-payload no-address message with capture that doesn't fit in address bits
template< typename H >
struct NTMessageSpecializer<H, false, false > {
  static void send_ntmessage( Core destination, H handler ) {
    LOG(INFO) << "No address; handler of size " << sizeof(H) << " too big for address field: " << __PRETTY_FUNCTION__;
    handler();
  }
};

//
// Messages with address but without payload
//


// helper struct to check operator() type if H has class type (functor or lambda)
template< typename H, typename ARG >
struct NTAddressMessageHelper :
    std::integral_constant< bool,
                            ( std::is_same< decltype( &H::operator() ), void (H::*)(ARG) const >::value ||
                              std::is_same< decltype( &H::operator() ), void (H::*)(ARG) >::value ) > { };

// handler is function pointer, not functor or lambda; don't check operator() type
template< typename H, typename ARG >
struct NTAddressMessageHelper<H*,ARG> : std::false_type { };


template< typename T, // address type
          typename H, // handler type
          // if this is a function pointer or no-capture lambda, note whether it takes a pointer or reference argument
          bool cast_to_voidstarptr = (std::is_convertible<H,void(*)(T*)>::value),
          bool cast_to_voidstarref = (std::is_convertible<H,void(*)(T&)>::value),
          // if this is a functor or lambda with capture, note whether it takes a pointer or reference argument
          bool operator_takes_ptr  = (NTAddressMessageHelper<H,T*>::value),
          bool operator_takes_ref  = (NTAddressMessageHelper<H,T&>::value) >
struct NTAddressMessageSpecializer : public NTHeader {
  static void send_ntmessage( GlobalAddress<T> address, H handler );
  static char * deserialize_and_call( char * t );
};

// Specializer for no-payload no-capture message with address with pointer argument
template< typename T, typename H, bool dontcare1, bool dontcare2 >
struct NTAddressMessageSpecializer<T, H, true, false, dontcare1, dontcare2> {
  static void send_ntmessage( GlobalAddress<T> address, H handler ) {
    LOG(INFO) << "GlobalAddress; handler has pointer and empty capture: " << __PRETTY_FUNCTION__;
  }
};

// Specializer for no-payload no-capture message with address with reference argument
template< typename T, typename H, bool dontcare1, bool dontcare2 >
struct NTAddressMessageSpecializer<T, H, false, true, dontcare1, dontcare2> {
  static void send_ntmessage( GlobalAddress<T> address, H handler ) {
    LOG(INFO) << "GlobalAddress; handler has reference and empty capture: " << __PRETTY_FUNCTION__;
  }
};

// Specializer for no-payload message with address and capture with pointer argument
template< typename T, typename H >
struct NTAddressMessageSpecializer<T, H, false, false, true, false> {
  static void send_ntmessage( GlobalAddress<T> address, H handler ) {
    LOG(INFO) << "GlobalAddress; handler has pointer and non-empty capture: " << __PRETTY_FUNCTION__;
  }
};

// Specializer for no-payload message with address and capture with reference argument
template< typename T, typename H >
struct NTAddressMessageSpecializer<T, H, false, false, false, true> {
  static void send_ntmessage( GlobalAddress<T> address, H handler ) {
    LOG(INFO) << "GlobalAddress; handler has reference and non-empty capture: " << __PRETTY_FUNCTION__;
  }
};


//
// Messages with payload but without address
//

template< typename H, // handler type
          typename P, // payload type
          // is this a function pointer or no-capture lambda?
          bool cast_to_voidstarvoid = (std::is_convertible<H,void(*)(void)>::value),
          // if this is a lambda with capture, can we fit it entirely in the address bits?
          bool fits_in_address      = (sizeof(H) * 8 <= NTMESSAGE_ADDRESS_BITS) >
struct NTPayloadMessageSpecializer : public NTHeader {
  static void send_ntmessage( Core destination, P * p, size_t size, H handler );
  static char * deserialize_and_call( char * t );
};

// Specializer for no-capture no-address message with payload
template< typename H, typename P, bool dontcare >
struct NTPayloadMessageSpecializer<H, P, true, dontcare> {
  static void send_ntmessage( Core destination, P * p, size_t size, H handler ) {
    LOG(INFO) << "Payload with no address; handler has empty capture: " << __PRETTY_FUNCTION__;
  }
};

// Specializer for no-address message with payload and capture that fits in address bits
template< typename H, typename P >
struct NTPayloadMessageSpecializer<H, P, false, true > {
  static void send_ntmessage( Core destination, P * p, size_t size, H handler ) {
    LOG(INFO) << "Payload with no address; handler of size " << sizeof(H) << " fits in address field: " << __PRETTY_FUNCTION__;
  }
};

// Specializer for no-address message with payload and capture that does not fit in address bits
template< typename H, typename P >
struct NTPayloadMessageSpecializer<H, P, false, false > {
  static void send_ntmessage( Core destination, P * p, size_t size, H handler ) {
    LOG(INFO) << "Payload with no address; handler of size " << sizeof(H) << " too big for address field: " << __PRETTY_FUNCTION__;
  }
};

//
// Messages with address and payload
//

// helper struct to check operator() type if H has class type (functor or lambda)
template< typename H, typename ARG, typename P >
struct NTPayloadAddressMessageHelper :
    std::integral_constant< bool,
                            ( std::is_same< decltype( &H::operator() ), void (H::*)(ARG,P*,size_t) const >::value ||
                              std::is_same< decltype( &H::operator() ), void (H::*)(ARG,P*,size_t) >::value ) > { };

// handler is function pointer, not functor or lambda; don't check operator() type
template< typename H, typename ARG, typename P >
struct NTPayloadAddressMessageHelper<H*,ARG,P> : std::false_type { };


template< typename T, // address type
          typename H, // handler type
          typename P, // payload type
          // if this is a function pointer or no-capture lambda, note whether it takes a pointer or reference argument
          bool cast_to_voidstarptr = (std::is_convertible<H,void(*)(T*,P*,size_t)>::value),
          bool cast_to_voidstarref = (std::is_convertible<H,void(*)(T&,P*,size_t)>::value),
          // if this is a functor or lambda with capture, note whether it takes a pointer or reference argument
          bool operator_takes_ptr  = (NTPayloadAddressMessageHelper<H,T*,P>::value),
          bool operator_takes_ref  = (NTPayloadAddressMessageHelper<H,T&,P>::value) >
struct NTPayloadAddressMessageSpecializer : public NTHeader {
  static void send_ntmessage( GlobalAddress<T> address, P * payload, size_t count, H handler );
  static char * deserialize_and_call( char * t );
};

// Specializer for no-capture message with address and payload; lambda takes pointer
template< typename T, typename H, typename P, bool dontcare1, bool dontcare2 >
struct NTPayloadAddressMessageSpecializer<T, H, P, true, false, dontcare1, dontcare2> {
  static void send_ntmessage( GlobalAddress<T> address, P * payload, size_t count, H handler ) {
    LOG(INFO) << "Payload with GlobalAddress; handler takes pointer and has empty capture: " << __PRETTY_FUNCTION__;
  }
};

// Specializer for no-capture message with address and payload; lambda takes reference
template< typename T, typename H, typename P, bool dontcare1, bool dontcare2 >
struct NTPayloadAddressMessageSpecializer<T, H, P, false, true, dontcare1, dontcare2> {
  static void send_ntmessage( GlobalAddress<T> address, P * payload, size_t count, H handler ) {
    LOG(INFO) << "Payload with GlobalAddress; handler takes reference and has empty capture: " << __PRETTY_FUNCTION__;
  }
};

// Specializer for message with address, payload, and capture, lambda takes pointer
template< typename T, typename H, typename P >
struct NTPayloadAddressMessageSpecializer<T, H, P, false, false, true, false> {
  static void send_ntmessage( GlobalAddress<T> address, P * payload, size_t count, H handler ) {
    LOG(INFO) << "Payload with GlobalAddress; handler takes pointer and has non-empty capture: " << __PRETTY_FUNCTION__;
  }
};

// Specializer for message with address, payload, and capture, lambda takes reference
template< typename T, typename H, typename P >
struct NTPayloadAddressMessageSpecializer<T, H, P, false, false, false, true> {
  static void send_ntmessage( GlobalAddress<T> address, P * payload, size_t count, H handler ) {
    LOG(INFO) << "Payload with GlobalAddress; handler takes reference and has non-empty capture: " << __PRETTY_FUNCTION__;
  }
};

} // namespace impl

//
// NTMessage sending functions exposed to user
//

// These are placeholders; really these functions should be doing the following:
// * get pointer to aggregation buffer to store message
// * form header for message with correct deserialization pointer (or combine with previous)
// * copy header and lambda and/or payload into buffer as appropriate
// * update most-recently-used bits to note that the buffer has a message in it
// * if buffer has reeached capactiy, send it now.
// The API of the specializers will need to be updated for this.

/// Send message with no address and no payload. 
template< typename H >
void send_new_ntmessage( Core destination, H handler ) {
  // placeholder API; should be updated to serialize into aggregation buffer
  Grappa::impl::NTMessageSpecializer<H>::send_ntmessage( destination, handler );
}

/// Send message with address and no payload. 
template< typename T, typename H >
void send_new_ntmessage( GlobalAddress<T> address, H handler ) {
  // placeholder API; should be updated to serialize into aggregation buffer
  Grappa::impl::NTAddressMessageSpecializer<T,H>::send_ntmessage( address, handler );
}

/// Send message with payload. Payload is copied, so payload buffer can be immediately reused.
template< typename H, typename P >
void send_new_ntmessage( Core destination, P * payload, size_t count, H handler ) {
  // placeholder API; should be updated to serialize into aggregation buffer
  Grappa::impl::NTPayloadMessageSpecializer<H,P>::send_ntmessage( destination, payload, count, handler );
}

/// Send message with address and payload. Payload is copied, so payload buffer can be immediately reused.
template< typename T, typename H, typename P >
void send_new_ntmessage( GlobalAddress<T> address, P * payload, size_t count, H handler ) {
  // placeholder API; should be updated to serialize into aggregation buffer
  Grappa::impl::NTPayloadAddressMessageSpecializer<T,H,P>::send_ntmessage( address, payload, count, handler );
}

} // namespace Grappa