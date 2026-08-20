package store

import (
	"encoding/hex"
	"testing"
)

// The vectors below were produced by OpenSSL through the same call the server
// makes, EVP_PBE_scrypt with N of 2^14, r of 8, p of 1 and a key of 32 bytes,
// with the salt passed as the hexadecimal text rather than the bytes it
// spells. See server/src/signaling/authenticator.cpp.
//
// This is the test that fails if somebody tunes a cost parameter here, or
// decodes the salt before deriving. Both mistakes produce a hash that looks
// perfectly valid and that the C++ server rejects for every password, which is
// otherwise discovered by a person who can no longer log in.
func TestDerivedKeyMatchesTheServer(t *testing.T) {
	cases := []struct {
		name     string
		password string
		saltHex  string
		want     string
	}{
		{
			name:     "a passphrase",
			password: "correct horse battery staple",
			saltHex:  "0123456789abcdef0123456789abcdef",
			want:     "b9e4c0458defb164feba9d9ffbec86e4ddffb2a590e4afa11b8a170fbd151218",
		},
		{
			name:     "a short password",
			password: "hunter2",
			saltHex:  "abcdef0123456789abcdef0123456789",
			want:     "848dcfce427b9338dac7c21787db68c43e611400a9b9b8051ddc74ac4f56f4e7",
		},
	}

	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			got, err := DeriveKeyHex(testCase.password, testCase.saltHex)
			if err != nil {
				t.Fatalf("DeriveKeyHex: %v", err)
			}
			if got != testCase.want {
				t.Errorf("derived %s, the server derives %s", got, testCase.want)
			}
		})
	}
}

func TestDerivedKeyDependsOnTheSalt(t *testing.T) {
	first, err := DeriveKeyHex("same password", "0123456789abcdef0123456789abcdef")
	if err != nil {
		t.Fatalf("DeriveKeyHex: %v", err)
	}
	second, err := DeriveKeyHex("same password", "fedcba9876543210fedcba9876543210")
	if err != nil {
		t.Fatalf("DeriveKeyHex: %v", err)
	}
	if first == second {
		t.Error("two salts produced one hash, which is the property a salt exists to destroy")
	}
}

func TestRandomHexIsThirtyTwoCharactersOfHexadecimal(t *testing.T) {
	// Sixteen bytes is what the server uses for both an identifier and a salt,
	// and a document whose user_id is a different length is one an operator
	// cannot match against the ones the server wrote.
	value, err := RandomHex(16)
	if err != nil {
		t.Fatalf("RandomHex: %v", err)
	}
	if len(value) != 32 {
		t.Errorf("RandomHex(16) is %d characters, want 32", len(value))
	}
	if _, err := hex.DecodeString(value); err != nil {
		t.Errorf("RandomHex(16) is not hexadecimal: %v", err)
	}

	other, err := RandomHex(16)
	if err != nil {
		t.Fatalf("RandomHex: %v", err)
	}
	if value == other {
		t.Error("two calls returned the same value")
	}
}

func TestNewCredentialsVerify(t *testing.T) {
	saltHex, hashHex, err := newCredentials("a password")
	if err != nil {
		t.Fatalf("newCredentials: %v", err)
	}

	derived, err := DeriveKeyHex("a password", saltHex)
	if err != nil {
		t.Fatalf("DeriveKeyHex: %v", err)
	}
	if derived != hashHex {
		t.Error("the stored hash is not what the password derives to, so nobody could log in")
	}

	wrong, err := DeriveKeyHex("another password", saltHex)
	if err != nil {
		t.Fatalf("DeriveKeyHex: %v", err)
	}
	if wrong == hashHex {
		t.Error("a different password derived the same hash")
	}
}
