package store

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"

	"go.mongodb.org/mongo-driver/v2/bson"
	"go.mongodb.org/mongo-driver/v2/mongo"
	"go.mongodb.org/mongo-driver/v2/mongo/options"
)

// What can go wrong in a way the screen has words for. Everything else is
// reported as it arrived from the driver, because a database that is down says
// so better than any sentence written here in advance.
var (
	// ErrUserExists is a username that is already taken.
	ErrUserExists = errors.New("username is already taken")
	// ErrUserNotFound is an account that is not there, which after a delete
	// from a second terminal is a normal thing to run into.
	ErrUserNotFound = errors.New("no account with that identifier")
	// ErrLastAdmin refuses the two ways to end up with a system nobody can
	// administer: deleting the last administrator, and demoting them.
	ErrLastAdmin = errors.New("the last administrator cannot be removed or demoted")
	// ErrEmptyCredentials matches the server's own refusal.
	ErrEmptyCredentials = errors.New("username and password must not be empty")
	// ErrAuditNotWritten wraps the result of an action that happened and was
	// not recorded. The action is not rolled back, for the reason
	// docs/security-review.md gives: refusing an administrative change because
	// the log is unreachable protects the log at the expense of the thing the
	// log is about. It is reported loudly instead.
	ErrAuditNotWritten = errors.New("the audit entry could not be written")
)

// How many audit entries a query reads.
//
// Larger than the server's own limits in server/src/store/audit_log.hpp, which
// exist so that a signaling server does not read an ever growing collection
// with its lock held. Nothing here holds a lock the server needs, and reading
// history is most of what this program is for.
const (
	DefaultAuditLimit = 200
	MaxAuditLimit     = 2000
)

// ClampAuditLimit puts a requested limit into the range above.
func ClampAuditLimit(requested int) int {
	if requested <= 0 {
		return DefaultAuditLimit
	}
	if requested > MaxAuditLimit {
		return MaxAuditLimit
	}
	return requested
}

// Config is what Open needs to reach a database.
type Config struct {
	URI      string
	Database string
	// How long any single operation may take. Short, so that an unreachable
	// database is a message on the status line rather than a frozen screen.
	Timeout time.Duration
	// Who the audit entries this session writes are attributed to.
	Actor Actor
}

// Store is the users and audit collections of one PartyShare database.
//
// Deliberately not the rooms collection: this program manages accounts and the
// record of what was done to them, and a room is a thing the server creates
// and destroys while people are inside it.
//
// The schema belongs to the server. Nothing here creates a collection or an
// index on purpose, so that pointing this tool at a mistyped database name
// leaves an empty database empty instead of furnishing it.
type Store struct {
	client   *mongo.Client
	users    *mongo.Collection
	audit    *mongo.Collection
	actor    Actor
	timeout  time.Duration
	endpoint string
}

// Open connects and confirms the database answers.
//
// The three timeouts are the ones mongo_store.cpp puts into its URI, for the
// same reason: server selection alone leaves an established connection that
// stopped answering waiting on the driver's defaults, which are measured in
// tens of seconds.
func Open(ctx context.Context, config Config) (*Store, error) {
	if config.Timeout <= 0 {
		config.Timeout = 2 * time.Second
	}

	clientOptions := options.Client().
		ApplyURI(config.URI).
		SetServerSelectionTimeout(config.Timeout).
		SetConnectTimeout(config.Timeout).
		SetTimeout(config.Timeout).
		SetAppName("partyshare-dbadmin")

	client, err := mongo.Connect(clientOptions)
	if err != nil {
		return nil, fmt.Errorf("could not connect to %s: %w", config.URI, err)
	}

	pingContext, cancel := context.WithTimeout(ctx, config.Timeout)
	defer cancel()
	if err := client.Ping(pingContext, nil); err != nil {
		// Disconnecting here rather than leaving it to the caller: a Store is
		// never returned alongside an error, so nobody else holds the client.
		_ = client.Disconnect(context.Background())
		return nil, fmt.Errorf("could not reach %s: %w", config.URI, err)
	}

	database := client.Database(config.Database)
	return &Store{
		client:   client,
		users:    database.Collection("users"),
		audit:    database.Collection("audit"),
		actor:    config.Actor,
		timeout:  config.Timeout,
		endpoint: describeEndpoint(config.URI),
	}, nil
}

// Close releases the connection.
func (s *Store) Close() error {
	closeContext, cancel := context.WithTimeout(context.Background(), s.timeout)
	defer cancel()
	return s.client.Disconnect(closeContext)
}

// Endpoint is the host part of the URI, without any credentials in it, for the
// header line. A password in a URI is a password on somebody's screen.
func (s *Store) Endpoint() string { return s.endpoint }

// Database is the name of the database this Store is bound to.
func (s *Store) Database() string { return s.users.Database().Name() }

// Actor is who this session writes audit entries as.
func (s *Store) Actor() Actor { return s.actor }

func (s *Store) scoped(ctx context.Context) (context.Context, context.CancelFunc) {
	return context.WithTimeout(ctx, s.timeout)
}

// Accounts is every account, oldest first, which is the order the server's own
// user list uses.
func (s *Store) Accounts(ctx context.Context) ([]Account, error) {
	scopedContext, cancel := s.scoped(ctx)
	defer cancel()

	cursor, err := s.users.Find(scopedContext, bson.D{},
		options.Find().SetSort(bson.D{{Key: "created_at", Value: 1}}))
	if err != nil {
		return nil, fmt.Errorf("could not list the accounts: %w", err)
	}

	var accounts []Account
	if err := cursor.All(scopedContext, &accounts); err != nil {
		return nil, fmt.Errorf("could not read the accounts: %w", err)
	}
	for i := range accounts {
		// A document written by hand, or by a version of the server older than
		// roles, has no role field at all. Normalising here is what keeps that
		// account out of the administrators count.
		accounts[i].Role = RoleFromString(string(accounts[i].Role))
	}
	return accounts, nil
}

// FindAccount reads one account by its identifier.
func (s *Store) FindAccount(ctx context.Context, userID string) (Account, error) {
	scopedContext, cancel := s.scoped(ctx)
	defer cancel()
	return s.findAccount(scopedContext, userID)
}

func (s *Store) findAccount(ctx context.Context, userID string) (Account, error) {
	var account Account
	err := s.users.FindOne(ctx, bson.D{{Key: "user_id", Value: userID}}).Decode(&account)
	if errors.Is(err, mongo.ErrNoDocuments) {
		return Account{}, ErrUserNotFound
	}
	if err != nil {
		return Account{}, fmt.Errorf("could not read the account: %w", err)
	}
	account.Role = RoleFromString(string(account.Role))
	return account, nil
}

// Summary counts what the header shows.
func (s *Store) Summary(ctx context.Context) (Summary, error) {
	scopedContext, cancel := s.scoped(ctx)
	defer cancel()

	users, err := s.users.CountDocuments(scopedContext, bson.D{})
	if err != nil {
		return Summary{}, fmt.Errorf("could not count the accounts: %w", err)
	}
	admins, err := s.users.CountDocuments(scopedContext,
		bson.D{{Key: "role", Value: string(RoleAdmin)}})
	if err != nil {
		return Summary{}, fmt.Errorf("could not count the administrators: %w", err)
	}
	entries, err := s.audit.CountDocuments(scopedContext, bson.D{})
	if err != nil {
		return Summary{}, fmt.Errorf("could not count the audit entries: %w", err)
	}
	return Summary{Users: users, Admins: admins, AuditEntries: entries}, nil
}

// countAdmins is the guard behind ErrLastAdmin.
//
// A failure answers one, never zero, exactly as MongoUserStore::count_with_role
// does: a count that cannot be taken has to read as "there might be only one",
// because zero would let the rule pass and the last administrator go.
func (s *Store) countAdmins(ctx context.Context) int64 {
	count, err := s.users.CountDocuments(ctx, bson.D{{Key: "role", Value: string(RoleAdmin)}})
	if err != nil {
		return 1
	}
	return count
}

// NewAccount is what the create form fills in.
type NewAccount struct {
	Username    string
	Password    string
	DisplayName string
	Avatar      string
	Role        Role
}

// CreateAccount adds an account and records it.
//
// The returned Account carries the generated identifier, and its credential
// fields are the ones just written, which is what lets the caller show the new
// row without a round trip.
func (s *Store) CreateAccount(ctx context.Context, spec NewAccount) (Account, error) {
	if strings.TrimSpace(spec.Username) == "" || spec.Password == "" {
		return Account{}, ErrEmptyCredentials
	}

	userID, err := RandomHex(16)
	if err != nil {
		return Account{}, err
	}
	saltHex, hashHex, err := newCredentials(spec.Password)
	if err != nil {
		return Account{}, err
	}

	account := Account{
		UserID:          userID,
		Username:        strings.TrimSpace(spec.Username),
		DisplayName:     strings.TrimSpace(spec.DisplayName),
		Avatar:          strings.TrimSpace(spec.Avatar),
		Role:            RoleFromString(string(spec.Role)),
		SaltHex:         saltHex,
		PasswordHashHex: hashHex,
		CreatedAt:       time.Now().Unix(),
	}
	if account.DisplayName == "" {
		// The server's own fallback, so that an account created here looks
		// like one created through the admin panel.
		account.DisplayName = account.Username
	}

	scopedContext, cancel := s.scoped(ctx)
	defer cancel()

	// Checked before the insert as well as by the unique index the server
	// creates. The index is what makes it true under a second terminal; this
	// is what makes the answer a sentence instead of a driver error.
	if err := s.users.FindOne(scopedContext,
		bson.D{{Key: "username", Value: account.Username}}).Err(); err == nil {
		return Account{}, ErrUserExists
	} else if !errors.Is(err, mongo.ErrNoDocuments) {
		return Account{}, fmt.Errorf("could not check the username: %w", err)
	}

	if _, err := s.users.InsertOne(scopedContext, account); err != nil {
		if mongo.IsDuplicateKeyError(err) {
			return Account{}, ErrUserExists
		}
		return Account{}, fmt.Errorf("could not create the account: %w", err)
	}

	return account, s.record(scopedContext, ActionCreateUser, account.UserID,
		"username="+account.Username+" role="+string(account.Role))
}

// UpdateAccount writes the editable fields of an account: the username, the
// display name, the avatar and the role.
//
// Not the credentials, which SetPassword owns, and not the creation time,
// which records when the account came into existence and is nobody's to
// rewrite. The audit detail names what actually changed, so an entry that says
// "role=admin" means the role moved and not that a form was submitted with the
// role it already had.
func (s *Store) UpdateAccount(ctx context.Context, updated Account) error {
	scopedContext, cancel := s.scoped(ctx)
	defer cancel()

	current, err := s.findAccount(scopedContext, updated.UserID)
	if err != nil {
		return err
	}

	updated.Username = strings.TrimSpace(updated.Username)
	updated.DisplayName = strings.TrimSpace(updated.DisplayName)
	updated.Avatar = strings.TrimSpace(updated.Avatar)
	updated.Role = RoleFromString(string(updated.Role))
	if updated.Username == "" {
		return ErrEmptyCredentials
	}
	if updated.DisplayName == "" {
		updated.DisplayName = updated.Username
	}

	if updated.Role != current.Role && current.IsAdmin() && s.countAdmins(scopedContext) <= 1 {
		return ErrLastAdmin
	}

	if updated.Username != current.Username {
		err := s.users.FindOne(scopedContext, bson.D{
			{Key: "username", Value: updated.Username},
			{Key: "user_id", Value: bson.D{{Key: "$ne", Value: updated.UserID}}},
		}).Err()
		if err == nil {
			return ErrUserExists
		}
		if !errors.Is(err, mongo.ErrNoDocuments) {
			return fmt.Errorf("could not check the username: %w", err)
		}
	}

	result, err := s.users.UpdateOne(scopedContext,
		bson.D{{Key: "user_id", Value: updated.UserID}},
		bson.D{{Key: "$set", Value: bson.D{
			{Key: "username", Value: updated.Username},
			{Key: "display_name", Value: updated.DisplayName},
			{Key: "avatar", Value: updated.Avatar},
			{Key: "role", Value: string(updated.Role)},
		}}})
	if err != nil {
		if mongo.IsDuplicateKeyError(err) {
			return ErrUserExists
		}
		return fmt.Errorf("could not update the account: %w", err)
	}
	if result.MatchedCount == 0 {
		return ErrUserNotFound
	}

	return s.record(scopedContext, ActionUpdateUser, updated.UserID, changes(current, updated))
}

// changes names the fields that moved, in the server's own vocabulary, so that
// "role=admin username" reads the same whichever program wrote it.
func changes(current, updated Account) string {
	var parts []string
	if updated.Role != current.Role {
		parts = append(parts, "role="+string(updated.Role))
	}
	if updated.Username != current.Username {
		parts = append(parts, "username="+updated.Username)
	}
	if updated.DisplayName != current.DisplayName {
		parts = append(parts, "display_name")
	}
	if updated.Avatar != current.Avatar {
		parts = append(parts, "avatar")
	}
	if len(parts) == 0 {
		return "no change"
	}
	return strings.Join(parts, " ")
}

// SetRestrictions writes what an administrator has taken away from an account,
// and records which of the four flags moved.
//
// The whole set is written, because that is what the caller decided: the form
// this comes from shows all four at once and the operator has just looked at
// each of them. That is the difference from the client's per participant
// shortcuts, which send one flag and leave the rest absent so as not to lift
// somebody else's decision by accident.
//
// The last administrator cannot be banned, for the reason they cannot be
// deleted or demoted: this program is the hand that recovers a system nobody
// can administer, and it should not be the hand that creates one. The other
// three are allowed on any account, an administrator's included: an
// administrator who may not use a microphone can still administer.
//
// What it deliberately does not do is what the server does around the same
// change: end the session, take the microphone, stop a share that is running.
// A running server holds all three in memory and a database tool reaches none
// of them. The consequence is stated on the screen rather than hidden here.
// The restriction itself is read back by the server on the account's next
// message, so it takes effect either way.
func (s *Store) SetRestrictions(ctx context.Context, userID string, restrictions Restrictions) error {
	scopedContext, cancel := s.scoped(ctx)
	defer cancel()

	current, err := s.findAccount(scopedContext, userID)
	if err != nil {
		return err
	}

	if restrictions == current.Restrictions {
		// Nothing written and nothing recorded. A form submitted with the
		// boxes it already had ticked is not an administrative action, and an
		// audit log full of those is a log nobody reads.
		return nil
	}
	if restrictions.Banned && !current.Restrictions.Banned &&
		current.IsAdmin() && s.countAdmins(scopedContext) <= 1 {
		return ErrLastAdmin
	}

	result, err := s.users.UpdateOne(scopedContext,
		bson.D{{Key: "user_id", Value: userID}},
		bson.D{{Key: "$set", Value: bson.D{{Key: "restrictions", Value: restrictions}}}})
	if err != nil {
		return fmt.Errorf("could not update the restrictions: %w", err)
	}
	if result.MatchedCount == 0 {
		return ErrUserNotFound
	}

	return s.record(scopedContext, ActionRestrictUser, userID,
		restrictionChanges(current.Restrictions, restrictions))
}

// restrictionChanges names the flags that moved and what they became, in the
// vocabulary the server writes: "banned=true muted=false" reads the same way
// "role=admin username" does, whichever program wrote it.
func restrictionChanges(current, updated Restrictions) string {
	var parts []string
	if updated.Banned != current.Banned {
		parts = append(parts, "banned="+boolText(updated.Banned))
	}
	if updated.Muted != current.Muted {
		parts = append(parts, "muted="+boolText(updated.Muted))
	}
	if updated.Silenced != current.Silenced {
		parts = append(parts, "silenced="+boolText(updated.Silenced))
	}
	if updated.ScreenShareBlocked != current.ScreenShareBlocked {
		parts = append(parts, "screen_share_blocked="+boolText(updated.ScreenShareBlocked))
	}
	if len(parts) == 0 {
		return "no change"
	}
	return strings.Join(parts, " ")
}

func boolText(value bool) string {
	if value {
		return "true"
	}
	return "false"
}

// SetPassword derives a fresh salt and hash and writes both.
//
// One write and not two, so that a failure cannot leave an account whose salt
// moved and whose hash did not, which is an account nobody can log in to.
func (s *Store) SetPassword(ctx context.Context, userID, password string) error {
	if password == "" {
		return ErrEmptyCredentials
	}

	saltHex, hashHex, err := newCredentials(password)
	if err != nil {
		return err
	}

	scopedContext, cancel := s.scoped(ctx)
	defer cancel()

	result, err := s.users.UpdateOne(scopedContext,
		bson.D{{Key: "user_id", Value: userID}},
		bson.D{{Key: "$set", Value: bson.D{
			{Key: "salt_hex", Value: saltHex},
			{Key: "password_hash_hex", Value: hashHex},
		}}})
	if err != nil {
		return fmt.Errorf("could not update the password: %w", err)
	}
	if result.MatchedCount == 0 {
		return ErrUserNotFound
	}

	return s.record(scopedContext, ActionUpdateUser, userID, "password")
}

// DeleteAccount removes an account and records it.
//
// What it deliberately does not do is what the server does around the same
// action: evict the account from its room and revoke its tokens. A running
// server holds both in memory, and a database tool cannot reach either. The
// consequence is stated on the confirmation screen rather than hidden here: a
// session opened before the delete keeps working until it expires.
func (s *Store) DeleteAccount(ctx context.Context, userID string) error {
	scopedContext, cancel := s.scoped(ctx)
	defer cancel()

	account, err := s.findAccount(scopedContext, userID)
	if err != nil {
		return err
	}
	if account.IsAdmin() && s.countAdmins(scopedContext) <= 1 {
		return ErrLastAdmin
	}

	result, err := s.users.DeleteOne(scopedContext, bson.D{{Key: "user_id", Value: userID}})
	if err != nil {
		return fmt.Errorf("could not delete the account: %w", err)
	}
	if result.DeletedCount == 0 {
		return ErrUserNotFound
	}

	return s.record(scopedContext, ActionDeleteUser, userID, "username="+account.Username)
}

// AuditQuery is what the audit screen asks for.
type AuditQuery struct {
	// Clamped by ClampAuditLimit.
	Limit int
	// Empty means every actor, which is the server's own convention.
	ActorID string
}

// auditDocument is AuditEntry plus the identifier MongoDB assigns.
//
// A separate type because AuditEntry is what the rest of the program handles,
// and it has no business carrying a driver type around for the sake of one
// column that shows eight characters of it.
type auditDocument struct {
	ID         bson.ObjectID `bson:"_id"`
	AuditEntry `bson:",inline"`
}

// Audit reads the log, newest first.
func (s *Store) Audit(ctx context.Context, query AuditQuery) ([]AuditEntry, error) {
	scopedContext, cancel := s.scoped(ctx)
	defer cancel()

	filter := bson.D{}
	if query.ActorID != "" {
		filter = bson.D{{Key: "actor_id", Value: query.ActorID}}
	}

	cursor, err := s.audit.Find(scopedContext, filter,
		options.Find().
			SetSort(bson.D{{Key: "timestamp_seconds", Value: -1}}).
			SetLimit(int64(ClampAuditLimit(query.Limit))))
	if err != nil {
		return nil, fmt.Errorf("could not read the audit log: %w", err)
	}

	var documents []auditDocument
	if err := cursor.All(scopedContext, &documents); err != nil {
		return nil, fmt.Errorf("could not read the audit log: %w", err)
	}

	entries := make([]AuditEntry, 0, len(documents))
	for _, document := range documents {
		entry := document.AuditEntry
		entry.ID = document.ID.Hex()
		entries = append(entries, entry)
	}
	return entries, nil
}

// record appends the audit entry for an action that already happened.
//
// The returned error wraps ErrAuditNotWritten and never means the action was
// undone. The caller shows it as a warning next to a change that stands.
func (s *Store) record(ctx context.Context, action, targetID, detail string) error {
	entry := AuditEntry{
		ActorID:          s.actor.ID,
		ActorUsername:    s.actor.Username,
		Action:           action,
		TargetID:         targetID,
		Detail:           detail,
		TimestampSeconds: time.Now().Unix(),
	}

	// Its own deadline: the operation that led here may have used most of the
	// caller's, and an entry that is dropped because there were fifty
	// milliseconds left is the hole the log exists to close.
	appendContext, cancel := context.WithTimeout(context.WithoutCancel(ctx), s.timeout)
	defer cancel()

	if _, err := s.audit.InsertOne(appendContext, bson.D{
		{Key: "actor_id", Value: entry.ActorID},
		{Key: "actor_username", Value: entry.ActorUsername},
		{Key: "action", Value: entry.Action},
		{Key: "target_id", Value: entry.TargetID},
		{Key: "room_id", Value: entry.RoomID},
		{Key: "detail", Value: entry.Detail},
		{Key: "timestamp_seconds", Value: entry.TimestampSeconds},
	}); err != nil {
		return fmt.Errorf("%w: %s", ErrAuditNotWritten, err)
	}
	return nil
}

// describeEndpoint reduces a URI to what belongs on a screen: the hosts, with
// any username and password taken out of it.
func describeEndpoint(uri string) string {
	trimmed := uri
	for _, scheme := range []string{"mongodb+srv://", "mongodb://"} {
		if after, found := strings.CutPrefix(trimmed, scheme); found {
			trimmed = after
			break
		}
	}
	if _, after, found := strings.Cut(trimmed, "@"); found {
		trimmed = after
	}
	if before, _, found := strings.Cut(trimmed, "/"); found {
		trimmed = before
	}
	if before, _, found := strings.Cut(trimmed, "?"); found {
		trimmed = before
	}
	if trimmed == "" {
		return uri
	}
	return trimmed
}
