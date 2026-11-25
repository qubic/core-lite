// Lite-only tests of the request a guardian signs to have a node score one ant solution.
#define NO_UEFI

#include "gtest/gtest.h"

#include "../src/platform/memory.h"
#include "../src/four_q.h"
#include "../src/extensions/ant_solution_check.h"

#include <cstring>
#include <string>

namespace
{
struct TestSigner
{
    m256i subseed;
    m256i publicKey;

    explicit TestSigner(char letter)
    {
        unsigned char seed[55];
        memset(seed, letter, sizeof(seed));
        m256i privateKey;
        EXPECT_TRUE(getSubseed(seed, subseed.m256i_u8));
        getPrivateKey(subseed.m256i_u8, privateKey.m256i_u8);
        getPublicKey(privateKey.m256i_u8, publicKey.m256i_u8);
    }
};

m256i patternKey(unsigned char first)
{
    m256i key;
    for (unsigned int i = 0; i < 32; i++)
    {
        key.m256i_u8[i] = (unsigned char)(first + i);
    }
    return key;
}

std::string identityOf(const m256i& publicKey)
{
    CHAR16 wide[61] = {};
    getIdentity(publicKey.m256i_u8, wide, false);
    std::string identity;
    for (unsigned int i = 0; i < 60; i++)
    {
        identity.push_back((char)wide[i]);
    }
    return identity;
}
}

// The byte layout is the contract with the client, so it is pinned here byte by byte.
TEST(TestAntSolutionCheck, DigestCoversTheSeventySixBytesInOrder)
{
    const m256i publicKey = patternKey(1);
    const m256i nonce = patternKey(101);

    unsigned char expectedMessage[76];
    for (unsigned int i = 0; i < 32; i++)
    {
        expectedMessage[i] = (unsigned char)(1 + i);
        expectedMessage[32 + i] = (unsigned char)(101 + i);
    }
    const unsigned char numbers[12] = { 0x01, 0x02, 0x03, 0x04, 0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24 };
    memcpy(expectedMessage + 64, numbers, sizeof(numbers));
    unsigned char expectedDigest[32];
    KangarooTwelve(expectedMessage, sizeof(expectedMessage), expectedDigest, sizeof(expectedDigest));

    unsigned char digest[32];
    antChallengeDigest(publicKey, nonce, 0x04030201u, 0x14131211u, 0x24232221u, digest);
    EXPECT_EQ(memcmp(digest, expectedDigest, sizeof(digest)), 0);
}

TEST(TestAntSolutionCheck, OnlyTheGuardianSignatureOverTheExactRequestVerifies)
{
    const TestSigner guardian('a');
    const TestSigner stranger('b');
    const m256i publicKey = patternKey(7);
    const m256i nonce = patternKey(70);
    const unsigned int anchorTick = 92260090;
    const unsigned int parentTick = 92260080;
    const unsigned int parentIndex = 3;

    unsigned char digest[32];
    antChallengeDigest(publicKey, nonce, anchorTick, parentTick, parentIndex, digest);
    unsigned char signature[64];
    sign(guardian.subseed.m256i_u8, guardian.publicKey.m256i_u8, digest, signature);

    EXPECT_TRUE(antChallengeSignatureValid(guardian.publicKey, publicKey, nonce, anchorTick, parentTick, parentIndex, signature));

    EXPECT_FALSE(antChallengeSignatureValid(stranger.publicKey, publicKey, nonce, anchorTick, parentTick, parentIndex, signature));
    EXPECT_FALSE(antChallengeSignatureValid(guardian.publicKey, patternKey(8), nonce, anchorTick, parentTick, parentIndex, signature));
    EXPECT_FALSE(antChallengeSignatureValid(guardian.publicKey, publicKey, patternKey(71), anchorTick, parentTick, parentIndex, signature));
    EXPECT_FALSE(antChallengeSignatureValid(guardian.publicKey, publicKey, nonce, anchorTick + 1, parentTick, parentIndex, signature));
    EXPECT_FALSE(antChallengeSignatureValid(guardian.publicKey, publicKey, nonce, anchorTick, parentTick + 1, parentIndex, signature));
    EXPECT_FALSE(antChallengeSignatureValid(guardian.publicKey, publicKey, nonce, anchorTick, parentTick, parentIndex + 1, signature));

    signature[10] ^= 1;
    EXPECT_FALSE(antChallengeSignatureValid(guardian.publicKey, publicKey, nonce, anchorTick, parentTick, parentIndex, signature));
}

TEST(TestAntSolutionCheck, HexMustBeExactlyTheBytesAsked)
{
    unsigned char out[4] = {};
    ASSERT_TRUE(antParseHex("00ffA1b2", out, 4));
    EXPECT_EQ(out[0], 0x00);
    EXPECT_EQ(out[1], 0xFF);
    EXPECT_EQ(out[2], 0xA1);
    EXPECT_EQ(out[3], 0xB2);

    EXPECT_FALSE(antParseHex("", out, 4));
    EXPECT_FALSE(antParseHex("00ffa1", out, 4));
    EXPECT_FALSE(antParseHex("00ffa1b2c3", out, 4));
    EXPECT_FALSE(antParseHex("0x00ffa1", out, 4));
    EXPECT_FALSE(antParseHex("00ffa1bg", out, 4));
    EXPECT_FALSE(antParseHex("00 fa1b2", out, 4));
}

TEST(TestAntSolutionCheck, NumbersAreDigitsOnlyAndFitThirtyTwoBits)
{
    unsigned int value = 7;
    ASSERT_TRUE(antParseUnsigned("0", value));
    EXPECT_EQ(value, 0u);
    ASSERT_TRUE(antParseUnsigned("4294967295", value));
    EXPECT_EQ(value, 0xFFFFFFFFu);

    EXPECT_FALSE(antParseUnsigned("4294967296", value));
    EXPECT_FALSE(antParseUnsigned("99999999999", value));
    EXPECT_FALSE(antParseUnsigned("", value));
    EXPECT_FALSE(antParseUnsigned("-1", value));
    EXPECT_FALSE(antParseUnsigned("+1", value));
    EXPECT_FALSE(antParseUnsigned(" 1", value));
    EXPECT_FALSE(antParseUnsigned("1a", value));
}

TEST(TestAntSolutionCheck, IdentityNeedsSixtyLettersAndItsChecksum)
{
    const TestSigner signer('c');
    const std::string identity = identityOf(signer.publicKey);

    m256i parsed = m256i::zero();
    ASSERT_TRUE(antParseIdentity(identity, parsed));
    EXPECT_TRUE(parsed == signer.publicKey);

    std::string wrongChecksum = identity;
    wrongChecksum[59] = (wrongChecksum[59] == 'A') ? 'B' : 'A';
    EXPECT_FALSE(antParseIdentity(wrongChecksum, parsed));

    std::string lowerCase = identity;
    lowerCase[0] = (char)(lowerCase[0] - 'A' + 'a');
    EXPECT_FALSE(antParseIdentity(lowerCase, parsed));

    EXPECT_FALSE(antParseIdentity(identity.substr(0, 59), parsed));
    EXPECT_FALSE(antParseIdentity(identity + "A", parsed));
    EXPECT_FALSE(antParseIdentity("", parsed));
}
