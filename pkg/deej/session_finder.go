package deej

// SessionFinder represents an entity that can find all current audio sessions
type SessionFinder interface {
	GetAllSessions() ([]Session, error)

	Release() error

	SubscribeToSessionEvents(buffer int) (<-chan SessionEvent, func())
}

type SessionEventType int

const (
	SessionDiscovered SessionEventType = iota
)

type SessionEvent struct {
	Type    SessionEventType
	Session Session
}
