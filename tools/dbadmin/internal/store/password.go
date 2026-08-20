package store

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"

	"golang.org/x/crypto/scrypt"
)

// The cost parameters of the server's Authenticator, restated here because
// this program writes credentials the server has to be able to verify.
//
// They are not tunable. A password stored under different parameters is not a
// weaker password, it is a password nobody can log in with: the server derives
// the candidate with its own constants and compares the result byte for byte.
// If server/src/signaling/authenticator.cpp ever changes them, this file has
// to change in the same commit, and TestDerivedKeyMatchesTheServer is what
// fails until it does.
const (
	scryptCost        = 1 << 14 // N
	scryptBlockSize   = 8       // r
	scryptParallelism = 1       // p
	scryptKeyBytes    = 32
)

// DeriveKeyHex returns the stored form of a password.
//
// The salt passed to scrypt is the hexadecimal text itself, all
// len(saltHex) bytes of it, and not the sixteen bytes it spells. That is what
// the C++ does, since it hands EVP_PBE_scrypt the std::string it holds, and a
// Go implementation that decoded the hex first would produce a hash the server
// rejects for every password.
func DeriveKeyHex(password, saltHex string) (string, error) {
	key, err := scrypt.Key([]byte(password), []byte(saltHex), scryptCost, scryptBlockSize,
		scryptParallelism, scryptKeyBytes)
	if err != nil {
		return "", fmt.Errorf("password key derivation failed: %w", err)
	}
	return hex.EncodeToString(key), nil
}

// RandomHex returns the hexadecimal form of n random bytes, which is how the
// server spells both an account identifier and a salt: sixteen bytes, thirty
// two characters.
func RandomHex(n int) (string, error) {
	buffer := make([]byte, n)
	if _, err := rand.Read(buffer); err != nil {
		// Without a working source of randomness nothing derived below can be
		// trusted, so this is reported rather than worked around.
		return "", fmt.Errorf("the system random number generator failed: %w", err)
	}
	return hex.EncodeToString(buffer), nil
}

// newCredentials derives a fresh salt and hash for a password.
//
// A new salt every time, never a reused one, for the reason the server states:
// two passwords of the same account sharing a salt is exactly the property a
// salt exists to destroy.
func newCredentials(password string) (saltHex, hashHex string, err error) {
	saltHex, err = RandomHex(16)
	if err != nil {
		return "", "", err
	}
	hashHex, err = DeriveKeyHex(password, saltHex)
	if err != nil {
		return "", "", err
	}
	return saltHex, hashHex, nil
}
