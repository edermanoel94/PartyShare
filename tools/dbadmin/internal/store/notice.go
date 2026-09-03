package store

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"

	"go.mongodb.org/mongo-driver/v2/bson"
)

// MaxNoticeTextBytes is the longest a notice may be, in bytes: the server's
// dv::models::kMaxNoticeTextBytes, and not tunable here for the reason the
// scrypt parameters are not. A longer one is refused rather than cut, because
// a message that arrives in half is worse than one that never arrived.
const MaxNoticeTextBytes = 500

var (
	// ErrEmptyNotice is a notice of nothing but whitespace, which would be a
	// box somebody has to dismiss with nothing in it.
	ErrEmptyNotice = errors.New("a notice must not be empty")
	// ErrNoticeTooLong is one the server would refuse over the wire.
	ErrNoticeTooLong = fmt.Errorf("a notice must fit in %d bytes", MaxNoticeTextBytes)
)

// Notice is one document of the notices collection: the Go transcription of
// dv::models::Notice, field name for field name with what
// server/src/store/mongo_store.cpp writes.
//
// The sender fields are carried because the server reads them, and they are
// empty for everything this program writes. A shell is not an account, so
// there is no identifier to put there, and a name invented for the occasion
// would be a name the recipient cannot look up. The client already says "an
// administrator" for an empty one, which is exactly what it says for a
// restriction written from here; the audit entry is where the operator's name
// is.
type Notice struct {
	// Assigned by MongoDB, the same way the server's own append takes it: the
	// hexadecimal object identifier is what the recipient acknowledges.
	ID              string `bson:"-"`
	UserID          string `bson:"user_id"`
	FromUserID      string `bson:"from_user_id"`
	FromDisplayName string `bson:"from_display_name"`
	Text            string `bson:"text"`
	// Seconds since the Unix epoch, UTC.
	CreatedAt int64 `bson:"created_at"`
	// Zero until the person says they read it. The server writes that half.
	AcknowledgedAt int64 `bson:"acknowledged_at"`
}

// SendNotice writes one notice to one account and records it.
//
// The document alone, and that is enough: a running server hands it over
// within a heartbeat if the person is connected and at their next sign-in if
// they are not, exactly as it does for one an administrator sent from the
// panel while they were away. What this program cannot do is the confirmation
// the panel gets - nobody is here to say it arrived - which is why the status
// line says "sent" and not "delivered".
func (s *Store) SendNotice(ctx context.Context, userID, text string) (Notice, error) {
	text = strings.TrimSpace(text)
	if text == "" {
		return Notice{}, ErrEmptyNotice
	}
	if len(text) > MaxNoticeTextBytes {
		return Notice{}, ErrNoticeTooLong
	}

	scopedContext, cancel := s.scoped(ctx)
	defer cancel()

	// The account has to exist. Not a formality, and the server makes the
	// same check: a notice to an identifier nobody holds would sit pending
	// forever, and an entry naming it would be an entry about nobody.
	if _, err := s.findAccount(scopedContext, userID); err != nil {
		return Notice{}, err
	}

	notice := Notice{
		UserID:    userID,
		Text:      text,
		CreatedAt: time.Now().Unix(),
	}
	result, err := s.notices.InsertOne(scopedContext, notice)
	if err != nil {
		return Notice{}, fmt.Errorf("could not write the notice: %w", err)
	}
	if id, ok := result.InsertedID.(bson.ObjectID); ok {
		notice.ID = id.Hex()
	}

	// The same detail the server writes for its own send_notice, identifier
	// and text, so that the two read alike in the one log they share.
	return notice, s.record(scopedContext, ActionSendNotice, userID, "notice="+notice.ID+" "+text)
}
