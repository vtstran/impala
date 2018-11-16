// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <stdlib.h>
#include <stdio.h>
#include <iostream>

#include <boost/utility.hpp>
#include <math.h>

#include "testutil/gtest-util.h"
#include "util/bit-packing.inline.h"
#include "util/bit-stream-utils.inline.h"
#include "util/rle-encoding.h"

#include "common/names.h"

namespace impala {

const int MAX_WIDTH = BatchedBitReader::MAX_BITWIDTH;

TEST(BitArray, TestBool) {
  const int len = 8;
  uint8_t buffer[len];

  BitWriter writer(buffer, len);

  // Write alternating 0's and 1's
  for (int i = 0; i < 8; ++i) {
    bool result = writer.PutValue(static_cast<uint64_t>(i % 2), 1);
    EXPECT_TRUE(result);
  }
  writer.Flush();
  EXPECT_EQ((int)buffer[0], BOOST_BINARY(1 0 1 0 1 0 1 0));

  // Write 00110011
  for (int i = 0; i < 8; ++i) {
    bool result;
    switch (i) {
      case 0:
      case 1:
      case 4:
      case 5:
        result = writer.PutValue(0, 1);
        break;
      default:
        result = writer.PutValue(1, 1);
        break;
    }
    EXPECT_TRUE(result);
  }
  writer.Flush();

  // Validate the exact bit value
  EXPECT_EQ((int)buffer[0], BOOST_BINARY(1 0 1 0 1 0 1 0));
  EXPECT_EQ((int)buffer[1], BOOST_BINARY(1 1 0 0 1 1 0 0));

  // Use the reader and validate
  BatchedBitReader reader(buffer, len);

  // Ensure it returns the same results after Reset().
  for (int trial = 0; trial < 2; ++trial) {
    bool batch_vals[16];
    EXPECT_EQ(16, reader.UnpackBatch(1, 16, batch_vals));
    for (int i = 0; i < 8; ++i)  EXPECT_EQ(batch_vals[i], i % 2);

    for (int i = 0; i < 8; ++i) {
      switch (i) {
        case 0:
        case 1:
        case 4:
        case 5:
          EXPECT_EQ(batch_vals[8 + i], false);
          break;
        default:
          EXPECT_EQ(batch_vals[8 + i], true);
          break;
      }
    }
    reader.Reset(buffer, len);
  }
}

// Writes 'num_vals' values with width 'bit_width' and reads them back.
void TestBitArrayValues(int bit_width, int num_vals) {
  const int len = BitUtil::Ceil(bit_width * num_vals, 8);
  const int64_t mod = bit_width == 64 ? 1 : 1LL << bit_width;

  uint8_t buffer[(len > 0) ? len : 1];
  BitWriter writer(buffer, len);
  for (int i = 0; i < num_vals; ++i) {
    bool result = writer.PutValue(i % mod, bit_width);
    EXPECT_TRUE(result);
  }
  writer.Flush();
  EXPECT_EQ(writer.bytes_written(), len);

  BatchedBitReader reader(buffer, len);
  BatchedBitReader reader2(reader); // Test copy constructor.
  // Ensure it returns the same results after Reset().
  for (int trial = 0; trial < 2; ++trial) {
    // Unpack all values at once with one batched reader and in small batches with the
    // other batched reader.
    vector<int64_t> batch_vals(num_vals);
    const int BATCH_SIZE = 32;
    vector<int64_t> batch_vals2(BATCH_SIZE);
    EXPECT_EQ(num_vals,
        reader.UnpackBatch(bit_width, num_vals, batch_vals.data()));
    for (int i = 0; i < num_vals; ++i) {
      if (i % BATCH_SIZE == 0) {
        int num_to_unpack = min(BATCH_SIZE, num_vals - i);
        EXPECT_EQ(num_to_unpack,
           reader2.UnpackBatch(bit_width, num_to_unpack, batch_vals2.data()));
      }
      EXPECT_EQ(i % mod, batch_vals[i]);
      EXPECT_EQ(i % mod, batch_vals2[i % BATCH_SIZE]);
    }
    EXPECT_EQ(reader.bytes_left(), 0);
    EXPECT_EQ(reader2.bytes_left(), 0);
    reader.Reset(buffer, len);
    reader2.Reset(buffer, len);
  }
}

TEST(BitArray, TestValues) {
  for (int width = 0; width <= MAX_WIDTH; ++width) {
    TestBitArrayValues(width, 1);
    TestBitArrayValues(width, 2);
    // Don't write too many values
    TestBitArrayValues(width, (width < 12) ? (1 << width) : 4096);
    TestBitArrayValues(width, 1024);
  }
}

class RleTest : public ::testing::Test {
 protected:
  /// All the legal values for min_repeated_run_length to pass to RleEncoder() in tests.
  std::vector<int> legal_min_run_lengths_;

  virtual void SetUp() {
    for (int run_length = 0; run_length < RleEncoder::MAX_RUN_LENGTH_BUFFER;
         run_length += 8) {
      legal_min_run_lengths_.push_back(run_length);
    }
  }

  virtual void TearDown() {}

  /// Get many values from a batch RLE decoder using its low level functions.
  template <typename T>
  static bool GetRleValues(RleBatchDecoder<T>* decoder, int num_vals, T* vals) {
    int decoded = 0;
    // Decode repeated and literal runs until we've filled the output.
    while (decoded < num_vals) {
      if (decoder->NextNumRepeats() > 0) {
        EXPECT_EQ(0, decoder->NextNumLiterals());
        int num_repeats_to_output =
            min<int>(decoder->NextNumRepeats(), num_vals - decoded);
        T repeated_val = decoder->GetRepeatedValue(num_repeats_to_output);
        for (int i = 0; i < num_repeats_to_output; ++i) {
          *vals = repeated_val;
          ++vals;
        }
        decoded += num_repeats_to_output;
        continue;
      }
      int num_literals_to_output =
          min<int>(decoder->NextNumLiterals(), num_vals - decoded);
      if (num_literals_to_output == 0) return false;
      if (!decoder->GetLiteralValues(num_literals_to_output, vals)) return false;
      decoded += num_literals_to_output;
      vals += num_literals_to_output;
    }
    return true;
  }

  /// Get many values from a batch RLE decoder using its GetValues() function.
  template <typename T>
  static bool GetRleValuesBatched(RleBatchDecoder<T>* decoder, int num_vals, T* vals) {
    return num_vals == decoder->GetValues(num_vals, vals);
  }

  // Validates encoding of values by encoding and decoding them.
  // If expected_encoding != NULL, validates that the encoded buffer is
  // exactly 'expected_encoding'.
  // if expected_len is not -1, validates that is is the same as the encoded size (in
  // bytes).
  int ValidateRle(const vector<int>& values, int bit_width, uint8_t* expected_encoding,
      int expected_len, int min_repeated_run_length) {
    stringstream ss;
    ss << "bit_width=" << bit_width
       << " min_repeated_run_length_=" << min_repeated_run_length;
    const string& description = ss.str();
    const int len = 64 * 1024;
    uint8_t buffer[len];
    EXPECT_LE(expected_len, len);

    RleEncoder encoder(buffer, len, bit_width, min_repeated_run_length);

    int encoded_len = 0;
    for (int clear_count = 0; clear_count < 2; clear_count++) {
      if (clear_count >= 1) {
        // Check that we can reuse the encoder after calling Clear().
        encoder.Clear();
      }
      for (int i = 0; i < values.size(); ++i) {
        bool result = encoder.Put(values[i]);
        EXPECT_TRUE(result);
      }
      encoded_len = encoder.Flush();

      if (expected_len != -1) {
        EXPECT_EQ(encoded_len, expected_len);
      }
      if (expected_encoding != NULL) {
        EXPECT_TRUE(memcmp(buffer, expected_encoding, expected_len) == 0);
      }

      // Verify read.
      RleBatchDecoder<uint64_t> per_value_decoder(buffer, len, bit_width);
      RleBatchDecoder<uint64_t> per_run_decoder(buffer, len, bit_width);
      RleBatchDecoder<uint64_t> batch_decoder(buffer, len, bit_width);
      // Ensure it returns the same results after Reset().
      for (int trial = 0; trial < 2; ++trial) {
        for (int i = 0; i < values.size(); ++i) {
          uint64_t val;
          EXPECT_TRUE(per_value_decoder.GetSingleValue(&val)) << description;
          EXPECT_EQ(values[i], val) << description << " i=" << i << " trial=" << trial;
        }
        // Unpack everything at once from the other decoders.
        vector<uint64_t> decoded_values1(values.size());
        vector<uint64_t> decoded_values2(values.size());
        EXPECT_TRUE(GetRleValues(
            &per_run_decoder, decoded_values1.size(), decoded_values1.data()));
        EXPECT_TRUE(GetRleValuesBatched(
            &batch_decoder, decoded_values2.size(), decoded_values2.data()));
        for (int i = 0; i < values.size(); ++i) {
          EXPECT_EQ(values[i], decoded_values1[i]) << description << " i=" << i;
          EXPECT_EQ(values[i], decoded_values2[i]) << description << " i=" << i;
        }
        per_value_decoder.Reset(buffer, len, bit_width);
        per_run_decoder.Reset(buffer, len, bit_width);
        batch_decoder.Reset(buffer, len, bit_width);
      }
    }
    return encoded_len;
  }

  // ValidateRle on 'num_vals' values with width 'bit_width'. If 'value' != -1, that value
  // is used, otherwise alternating values are used.
  void TestRleValues(int bit_width, int num_vals, int value = -1) {
    const int64_t mod = (bit_width == 64) ? 1 : 1LL << bit_width;
    vector<int> values;
    for (int v = 0; v < num_vals; ++v) {
      values.push_back((value != -1) ? value : (v % mod));
    }
    for (auto min_run_length : legal_min_run_lengths_) {
      ValidateRle(values, bit_width, NULL, -1, min_run_length);
    }
    for (auto min_run_length : legal_min_run_lengths_) {
      ValidateRle(values, bit_width, NULL, -1, min_run_length);
    }
  }

  /// Returns the total number of bytes written when encoding the boolean values passed.
  int RleBooleanLength(const vector<int>& values, int min_repeated_run_length) {
    return ValidateRle(values, 1, nullptr, -1, min_repeated_run_length);
  }

  /// Make a sequence of values.
  /// literalLength1: the length of an initial literal sequence.
  /// repeatedLength: the length of a repeated sequence.
  /// literalLength2: the length of a closing literal sequence.
  vector<int>& MakeSequence(vector<int>& values, int intitial_literal_length,
      int repeated_length, int trailing_literal_length) {
    values.clear();
    for (int i = 0; i < intitial_literal_length; ++i) {
      values.push_back(i % 2);
    }
    for (int i = 0; i < repeated_length; ++i) {
      values.push_back(1);
    }
    for (int i = 0; i < trailing_literal_length; ++i) {
      values.push_back(i % 2);
    }
    return values;
  }
};

/// Basic test case for literal unpacking - two literals in a run.
TEST_F(RleTest, TwoLiteralRun) {
  vector<int> values{1, 0};
  for (auto min_run_length : legal_min_run_lengths_) {
    ValidateRle(values, 1, nullptr, -1, min_run_length);
    for (int width = 1; width <= MAX_WIDTH; ++width) {
      ValidateRle(values, width, nullptr, -1, min_run_length);
    }
  }
}

TEST_F(RleTest, SpecificSequences) {
  const int len = 1024;
  uint8_t expected_buffer[len];
  vector<int> values;

  // Test 50 0' followed by 50 1's
  values.resize(100);
  for (int i = 0; i < 50; ++i) {
    values[i] = 0;
  }
  for (int i = 50; i < 100; ++i) {
    values[i] = 1;
  }

  // expected_buffer valid for bit width <= 1 byte
  expected_buffer[0] = (50 << 1);
  expected_buffer[1] = 0;
  expected_buffer[2] = (50 << 1);
  expected_buffer[3] = 1;
  for (auto min_run_length : legal_min_run_lengths_) {
    for (int width = 1; width <= 8; ++width) {
      ValidateRle(values, width, expected_buffer, 4, min_run_length);
    }

    for (int width = 9; width <= MAX_WIDTH; ++width) {
      ValidateRle(values, width, NULL, 2 * (1 + BitUtil::Ceil(width, 8)), min_run_length);
    }
  }

  // Test 100 0's and 1's alternating
  for (int i = 0; i < 100; ++i) {
    values[i] = i % 2;
  }
  int num_groups = BitUtil::Ceil(100, 8);
  expected_buffer[0] = static_cast<uint8_t>((num_groups << 1) | 1);
  for (int i = 1; i <= 100/8; ++i) {
    expected_buffer[i] = BOOST_BINARY(1 0 1 0 1 0 1 0);
  }
  // Values for the last 4 0 and 1's. The upper 4 bits should be padded to 0.
  expected_buffer[100/8 + 1] = BOOST_BINARY(0 0 0 0 1 0 1 0);

  // num_groups and expected_buffer only valid for bit width = 1
  for (auto min_run_length : legal_min_run_lengths_) {
    ValidateRle(values, 1, expected_buffer, 1 + num_groups, min_run_length);
    for (int width = 2; width <= MAX_WIDTH; ++width) {
      int num_values = BitUtil::Ceil(100, 8) * 8;
      ValidateRle(
          values, width, NULL, 1 + BitUtil::Ceil(width * num_values, 8), min_run_length);
    }
  }

  for (auto min_run_length : legal_min_run_lengths_) {
    if (min_run_length == 0) continue; // Does not work with test logic.
    // A run of min_run_length 0's then a similar run of 1's then 0's then 1's.
    values.resize(static_cast<unsigned long>(4 * min_run_length));
    for (int i = 0; i < min_run_length; ++i) {
      values[i] = 0;
    }
    for (int i = min_run_length; i < 2 * min_run_length; ++i) {
      values[i] = 1;
    }
    for (int i = 2 * min_run_length; i < 3 * min_run_length; ++i) {
      values[i] = 0;
    }
    for (int i = 3 * min_run_length; i < 4 * min_run_length; ++i) {
      values[i] = 1;
    }
    // Expected_buffer valid for bit width <= 1 byte, and all values of min_run_length.
    expected_buffer[0] = (min_run_length << 1);
    expected_buffer[1] = 0;
    expected_buffer[2] = (min_run_length << 1);
    expected_buffer[3] = 1;
    expected_buffer[4] = (min_run_length << 1);
    expected_buffer[5] = 0;
    expected_buffer[6] = (min_run_length << 1);
    expected_buffer[7] = 1;

    for (int width = 1; width <= 8; ++width) {
      ValidateRle(values, width, expected_buffer, 8, min_run_length);
    }
  }

  // With min_run_length = 16 we will not encode a run of 8.
  values.clear();
  for (int i = 0; i < 32; ++i) {
    values.push_back(i % 2);
  }
  for (int i = 0; i < 8; ++i) {
    values.push_back(1);
  }
  for (int i = 0; i < 32; ++i) {
    values.push_back(i % 2);
  }
  expected_buffer[0] = (9 << 1 | 1); // 0x15;
  expected_buffer[1] = 0b10101010; // first bit is lsb, i.e. 0
  expected_buffer[2] = 0b10101010;
  expected_buffer[3] = 0b10101010;
  expected_buffer[4] = 0b10101010;
  expected_buffer[5] = 0b11111111;
  expected_buffer[6] = 0b10101010;
  expected_buffer[7] = 0b10101010;
  expected_buffer[8] = 0b10101010;
  expected_buffer[9] = 0b10101010;
  ValidateRle(values, 1, expected_buffer, 10, 16);
}

TEST_F(RleTest, TestValues) {
  for (int width = 1; width <= MAX_WIDTH; ++width) {
    TestRleValues(width, 1);
    TestRleValues(width, 1024);
    TestRleValues(width, 1024, 0);
    TestRleValues(width, 1024, 1);
  }
}

TEST_F(RleTest, BitWidthZeroRepeated) {
  uint8_t buffer[1];
  const int num_values = 15;
  buffer[0] = num_values << 1; // repeated indicator byte
  RleBatchDecoder<uint8_t> decoder(buffer, sizeof(buffer), 0);
  // Ensure it returns the same results after Reset().
  for (int trial = 0; trial < 2; ++trial) {
    uint8_t val;
    for (int i = 0; i < num_values; ++i) {
      EXPECT_TRUE(decoder.GetSingleValue(&val));
      EXPECT_EQ(val, 0);
    }
    EXPECT_FALSE(decoder.GetSingleValue(&val));

    // Test decoding all values in a batch.
    decoder.Reset(buffer, sizeof(buffer), 0);
    uint8_t decoded_values[num_values];
    EXPECT_TRUE(GetRleValues(&decoder, num_values, decoded_values));
    for (int i = 0; i < num_values; i++) EXPECT_EQ(0, decoded_values[i]) << i;
    EXPECT_FALSE(decoder.GetSingleValue(&val));
    decoder.Reset(buffer, sizeof(buffer), 0);
  }
}

TEST_F(RleTest, BitWidthZeroLiteral) {
  uint8_t buffer[1];
  const int num_groups = 4;
  buffer[0] = num_groups << 1 | 1; // literal indicator byte
  RleBatchDecoder<uint8_t> decoder(buffer, sizeof(buffer), 0);
  // Ensure it returns the same results after Reset().
  for (int trial = 0; trial < 2; ++trial) {
    const int num_values = num_groups * 8;
    uint8_t val;
    for (int i = 0; i < num_values; ++i) {
      EXPECT_TRUE(decoder.GetSingleValue(&val));
      EXPECT_EQ(val, 0); // can only encode 0s with bit width 0
    }

    // Test decoding the whole batch at once.
    decoder.Reset(buffer, sizeof(buffer), 0);
    uint8_t decoded_values[num_values];
    EXPECT_TRUE(GetRleValues(&decoder, num_values, decoded_values));
    for (int i = 0; i < num_values; ++i) EXPECT_EQ(0, decoded_values[i]);

    EXPECT_FALSE(GetRleValues(&decoder, 1, decoded_values));
    decoder.Reset(buffer, sizeof(buffer), 0);
  }
}

// Test that writes out a repeated group and then a literal
// group but flush before finishing.
TEST_F(RleTest, Flush) {
  for (auto min_run_length : legal_min_run_lengths_) {
    vector<int> values;
    for (int i = 0; i < 16; ++i) values.push_back(1);
    values.push_back(0);
    ValidateRle(values, 1, NULL, -1, min_run_length);

    for (int i = 0; i < min_run_length; ++i) {
      values.push_back(1);
      ValidateRle(values, 1, NULL, -1, min_run_length);
    }
  }
}

// Test some random sequences.
TEST_F(RleTest, Random) {
  int iters = 0;
  while (iters < 1000) {
    srand(static_cast<unsigned int>(iters++));
    if (iters % 10000 == 0) LOG(ERROR) << "Seed: " << iters;
    vector<int> values;
    bool parity = 0;
    for (int i = 0; i < 1000; ++i) {
      int group_size = rand() % 20 + 1;
      if (group_size > 16) {
        group_size = 1;
      }
      for (int j = 0; j < group_size; ++j) {
        values.push_back(parity);
      }
      parity = !parity;
    }
    for (auto min_run_length : legal_min_run_lengths_) {
      ValidateRle(values, (iters % MAX_WIDTH) + 1, NULL, -1, min_run_length);
    }
  }
}

// Test a sequence of 1 0's, 2 1's, 3 0's. etc
// e.g. 011000111100000
TEST_F(RleTest, RepeatedPattern) {
  vector<int> values;
  const int min_run = 1;
  const int max_run = 32;

  for (int i = min_run; i <= max_run; ++i) {
    int v = i % 2;
    for (int j = 0; j < i; ++j) {
      values.push_back(v);
    }
  }

  // And go back down again
  for (int i = max_run; i >= min_run; --i) {
    int v = i % 2;
    for (int j = 0; j < i; ++j) {
      values.push_back(v);
    }
  }
  for (auto min_run_length : legal_min_run_lengths_) {
    ValidateRle(values, 1, NULL, -1, min_run_length);
  }
}

TEST_F(RleTest, Overflow) {
  for (auto min_run_length : legal_min_run_lengths_) {
    for (int bit_width = 1; bit_width < 32; bit_width += 1) {
      for (int pad_buffer = 0; pad_buffer < 64; pad_buffer++) {
        const int len = RleEncoder::MinBufferSize(bit_width) + pad_buffer;
        uint8_t buffer[len];
        int num_added = 0;
        bool parity = true;

        RleEncoder encoder(buffer, len, bit_width, min_run_length);
        // Insert alternating true/false until there is no space left.
        while (true) {
          bool result = encoder.Put(static_cast<uint64_t>(parity));
          parity = !parity;
          if (!result) break;
          ++num_added;
        }

        int bytes_written = encoder.Flush();
        EXPECT_LE(bytes_written, len);
        EXPECT_GT(num_added, 0);

        RleBatchDecoder<uint32_t> decoder(buffer, bytes_written, bit_width);
        // Ensure it returns the same results after Reset().
        for (int trial = 0; trial < 2; ++trial) {
          parity = true;
          uint32_t v;
          for (int i = 0; i < num_added; ++i) {
            EXPECT_TRUE(decoder.GetSingleValue(&v));
            EXPECT_EQ(v, parity);
            parity = !parity;
          }
          // Make sure we get false when reading past end a couple times.
          EXPECT_FALSE(decoder.GetSingleValue(&v));
          EXPECT_FALSE(decoder.GetSingleValue(&v));

          decoder.Reset(buffer, bytes_written, bit_width);
          uint32_t decoded_values[num_added];
          EXPECT_TRUE(GetRleValues(&decoder, num_added, decoded_values));
          for (int i = 0; i < num_added; ++i)
            EXPECT_EQ(i % 2 == 0, decoded_values[i]) << i;

          decoder.Reset(buffer, bytes_written, bit_width);
        }
      }
    }
  }
}

/// Construct data sequences in the vector values for bit_widths of 1 and 2, such that
/// encoding using runs results in the encoding occupying more space than if the sequences
/// had been encoded using literal values.
void AddPathologicalValues(vector<int>& values, int bit_width) {
  EXPECT_LE(bit_width, 2);
  // Using the notation of 'RXX' for a repeated run of length XX (so R16 is a
  // run of length 16), and 'LYY' for a literal run of length YY.
  // For bit_width=1 the sequence (L8 R16 L8 R16 L8) is encoded as following
  // for different values of min_run_length.
  //                                  L8  R16 L8  R16 L8
  // min_run_length 8  (old default)  2   2   2   2   2
  // min_run_length 16 (new default)  2   2   2   2   2
  // min_run_length 24                2   2   1   2   1 (one long literal run)
  //
  // So it would have been better not to use rle for this sequence.
  // Note that min_run_length=24 is not *always* better.
  for (int i = 0; i < 8; i++) {
    // A literal sequence.
    values.push_back(i % 2);
  }
  // For bit_width = 2, a run of 8 values is needed.
  // For bit_width = 1, a run of 16 values is needed.
  int length_run = 16 / bit_width;
  // For small amounts of data, the value returned by MaxBufferSize is dominated by
  // MinBufferSize. Add enough data that this is not true.
  for (int runs = 0; runs < 200; runs++) {
    for (int i = 0; i < length_run; i++) {
      // A sequence that can be encoded as a run.
      values.push_back(1);
    }
    for (int i = 0; i < 8; i++) {
      // A literal sequence.
      values.push_back(i % 2);
    }
  }
}

/// Test that MaxBufferSize is accurate at low bit widths.
TEST_F(RleTest, MaxBufferSize) {
  const int bit_widths[] = {1, 2};
  for (auto bit_width : bit_widths) {
    vector<int> values;
    AddPathologicalValues(values, bit_width);
    const int expected_max_buffer_len =
        RleEncoder::MaxBufferSize(bit_width, values.size());

    // For the test to work we want enough values such that MaxBufferSize is not dominated
    // by MinBufferSize, check that this is true.
    EXPECT_GT(expected_max_buffer_len, RleEncoder::MinBufferSize(bit_width));

    // Allocate a buffer big enough that we won't hit buffer full.
    int big_buffer_len = expected_max_buffer_len * 10;
    uint8_t buffer[big_buffer_len];

    RleEncoder encoder(buffer, big_buffer_len, bit_width);

    int num_added = 0;
    for (int i = 0; i < values.size(); ++i) {
      bool result = encoder.Put(values[i]);
      EXPECT_TRUE(result) << "Failed to write after " << i << " values.";
      num_added++;
    }
    EXPECT_EQ(values.size(), num_added);

    int encoded_len = encoder.Flush();
    EXPECT_LE(encoded_len, expected_max_buffer_len)
        << "Encoded length was greater than MaxBufferSize for bit_width=" << bit_width;
  }
}

// Tests handling of a specific data corruption scenario where
// the literal or repeat count is decoded as 0 (which is invalid).
TEST_F(RleTest, ZeroLiteralOrRepeatCount) {
  const int len = 1024;
  uint8_t buffer[len];
  RleBatchDecoder<uint64_t> decoder(buffer, len, 0);
  // Test the RLE repeated values path.
  memset(buffer, 0, len);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(0, decoder.NextNumLiterals());
    EXPECT_EQ(0, decoder.NextNumRepeats());
  }

  // Test the RLE literal values path
  memset(buffer, 1, len);
  decoder.Reset(buffer, len, 0);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(0, decoder.NextNumLiterals());
    EXPECT_EQ(0, decoder.NextNumRepeats());
  }
}

// Regression test for handing of repeat counts >= 2^31: IMPALA-6946.
TEST_F(RleTest, RepeatCountOverflow) {
  const int BUFFER_LEN = 1024;
  uint8_t buffer[BUFFER_LEN];

  for (bool literal_run : {true, false}) {
    memset(buffer, 0, BUFFER_LEN);
    LOG(INFO) << "Testing negative " << (literal_run ? "literal" : "repeated");
    BitWriter writer(buffer, BUFFER_LEN);
    // Literal runs have lowest bit 1. Repeated runs have lowest bit 0. All other bits
    // are 1.
    const uint32_t REPEATED_RUN_HEADER = 0xfffffffe;
    const uint32_t LITERAL_RUN_HEADER = 0xffffffff;
    writer.PutUleb128Int(literal_run ? LITERAL_RUN_HEADER : REPEATED_RUN_HEADER);
    writer.Flush();

    RleBatchDecoder<uint64_t> decoder(buffer, BUFFER_LEN, 1);
    // Repeated run length fits in an int32_t.
    if (literal_run) {
      EXPECT_EQ(0, decoder.NextNumRepeats()) << "Not a repeated run";
      // Literal run length would overflow int32_t - should gracefully fail decoding.
      EXPECT_EQ(0, decoder.NextNumLiterals());
    } else {
      EXPECT_EQ(0x7fffffff, decoder.NextNumRepeats());
      EXPECT_EQ(0, decoder.NextNumLiterals()) << "Not a literal run";
    }

    // IMPALA-6946: reading back run lengths that don't fit in int32_t hit various
    // DCHECKs.
    uint64_t val;
    if (literal_run) {
      EXPECT_EQ(0, decoder.GetValues(1, &val)) << "Decoding failed above.";
    } else {
      EXPECT_EQ(1, decoder.GetValues(1, &val));
      EXPECT_EQ(0, val) << "Buffer was initialized with all zeroes";
    }
  }
}

/// Test that encoded lengths are as expected as min_repeated_run_length varies.
TEST_F(RleTest, MeasureOutputLengths) {
  vector<int> values;
  // With min_repeated_run_length = 8, a sequence of 8 is inefficient.
  EXPECT_EQ(12, RleBooleanLength(MakeSequence(values, 32, 8, 32), 8));
  EXPECT_EQ(12, RleBooleanLength(MakeSequence(values, 32, 16, 32), 8));
  EXPECT_EQ(12, RleBooleanLength(MakeSequence(values, 32, 24, 32), 8));
  EXPECT_EQ(12, RleBooleanLength(MakeSequence(values, 32, 32, 32), 8));

  // With min_repeated_run_length = 16, a sequence of 16 is inefficient.
  EXPECT_EQ(10, RleBooleanLength(MakeSequence(values, 32, 8, 32), 16));
  EXPECT_EQ(12, RleBooleanLength(MakeSequence(values, 32, 16, 32), 16));
  EXPECT_EQ(12, RleBooleanLength(MakeSequence(values, 32, 24, 32), 16));
  EXPECT_EQ(12, RleBooleanLength(MakeSequence(values, 32, 32, 32), 16));
}
}

IMPALA_TEST_MAIN();
