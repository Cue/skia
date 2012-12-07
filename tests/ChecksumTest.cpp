
/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "Test.h"
#include "SkChecksum.h"
#include "SkCityHash.h"

// Word size that is large enough to hold results of any checksum type.
typedef uint64_t checksum_result;

namespace skiatest {
    class ChecksumTestClass : public Test {
    public:
        static Test* Factory(void*) {return SkNEW(ChecksumTestClass); }
    protected:
        virtual void onGetName(SkString* name) { name->set("Checksum"); }
        virtual void onRun(Reporter* reporter) {
            this->fReporter = reporter;
            RunTest();
        }
    private:
        enum Algorithm {
            kSkChecksum,
            kSkCityHash32,
            kSkCityHash64
        };

        // Call Compute(data, size) on the appropriate checksum algorithm,
        // depending on this->fWhichAlgorithm.
        checksum_result ComputeChecksum(const char *data, size_t size) {
            switch(fWhichAlgorithm) {
            case kSkChecksum:
                REPORTER_ASSERT_MESSAGE(fReporter,
                                        reinterpret_cast<uintptr_t>(data) % 4 == 0,
                                        "test data pointer is not 32-bit aligned");
                REPORTER_ASSERT_MESSAGE(fReporter, SkIsAlign4(size),
                                        "test data size is not 32-bit aligned");
                return SkChecksum::Compute(reinterpret_cast<const uint32_t *>(data), size);
            case kSkCityHash32:
                return SkCityHash::Compute32(data, size);
            case kSkCityHash64:
                return SkCityHash::Compute64(data, size);
            default:
                SkString message("fWhichAlgorithm has unknown value ");
                message.appendf("%d", fWhichAlgorithm);
                fReporter->reportFailed(message);
            }
            // we never get here
            return 0;
        }

        // Confirm that the checksum algorithm (specified by fWhichAlgorithm)
        // generates the same results if called twice over the same data.
        void TestChecksumSelfConsistency(size_t buf_size) {
            SkAutoMalloc storage(buf_size);
            char* ptr = reinterpret_cast<char *>(storage.get());

            REPORTER_ASSERT(fReporter,
                            GetTestDataChecksum(8, 0) ==
                            GetTestDataChecksum(8, 0));
            REPORTER_ASSERT(fReporter,
                            GetTestDataChecksum(8, 0) !=
                            GetTestDataChecksum(8, 1));

            sk_bzero(ptr, buf_size);
            checksum_result prev = 0;

            // assert that as we change values (from 0 to non-zero) in
            // our buffer, we get a different value
            for (size_t i = 0; i < buf_size; ++i) {
                ptr[i] = (i & 0x7f) + 1; // need some non-zero value here

                // Try checksums of different-sized chunks, but always
                // 32-bit aligned and big enough to contain all the
                // nonzero bytes.  (Remaining bytes will still be zero
                // from the initial sk_bzero() call.)
                size_t checksum_size = (((i/4)+1)*4);
                REPORTER_ASSERT(fReporter, checksum_size <= buf_size);

                checksum_result curr = ComputeChecksum(ptr, checksum_size);
                REPORTER_ASSERT(fReporter, prev != curr);
                checksum_result again = ComputeChecksum(ptr, checksum_size);
                REPORTER_ASSERT(fReporter, again == curr);
                prev = curr;
            }
        }

        // Return the checksum of a buffer of bytes 'len' long.
        // The pattern of values within the buffer will be consistent
        // for every call, based on 'seed'.
        checksum_result GetTestDataChecksum(size_t len, char seed=0) {
            SkAutoMalloc storage(len);
            char* start = reinterpret_cast<char *>(storage.get());
            char* ptr = start;
            for (size_t i = 0; i < len; ++i) {
                *ptr++ = ((seed+i) & 0x7f);
            }
            checksum_result result = ComputeChecksum(start, len);
            return result;
        }

        void RunTest() {
            // Test self-consistency of checksum algorithms.
            fWhichAlgorithm = kSkChecksum;
            TestChecksumSelfConsistency(128);
            fWhichAlgorithm = kSkCityHash32;
            TestChecksumSelfConsistency(128);
            fWhichAlgorithm = kSkCityHash64;
            TestChecksumSelfConsistency(128);

            // Test checksum results that should be consistent across
            // versions and platforms.
            fWhichAlgorithm = kSkChecksum;
            REPORTER_ASSERT(fReporter, ComputeChecksum(NULL, 0) == 0);
            fWhichAlgorithm = kSkCityHash32;
            REPORTER_ASSERT(fReporter, ComputeChecksum(NULL, 0) == 0xdc56d17a);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(4)   == 0x616e1132);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(8)   == 0xeb0fd2d6);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(128) == 0x5321e430);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(132) == 0x924a10e4);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(256) == 0xd4de9dc9);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(260) == 0xecf0325d);
            fWhichAlgorithm = kSkCityHash64;
            REPORTER_ASSERT(fReporter, ComputeChecksum(NULL, 0) == 0x9ae16a3b2f90404fULL);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(4)   == 0x82bffd898958e540ULL);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(8)   == 0xad5a13e1e8e93b98ULL);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(128) == 0x10b153630af1f395ULL);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(132) == 0x7db71dc4adcc6647ULL);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(256) == 0xeee763519b91b010ULL);
            REPORTER_ASSERT(fReporter, GetTestDataChecksum(260) == 0x2fe19e0b2239bc23ULL);

            // TODO: note the weakness exposed by these collisions...
            // We need to improve the SkChecksum algorithm.
            // We would prefer that these asserts FAIL!
            // Filed as https://code.google.com/p/skia/issues/detail?id=981
            // ('SkChecksum algorithm allows for way too many collisions')
            fWhichAlgorithm = kSkChecksum;
            REPORTER_ASSERT(fReporter,
                GetTestDataChecksum(128) == GetTestDataChecksum(256));
            REPORTER_ASSERT(fReporter,
                GetTestDataChecksum(132) == GetTestDataChecksum(260));
        }

        Reporter* fReporter;
        Algorithm fWhichAlgorithm;
    };

    static TestRegistry gReg(ChecksumTestClass::Factory);
}
