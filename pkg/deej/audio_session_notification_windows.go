package deej

import (
	"syscall"
	"unsafe"

	"github.com/go-ole/go-ole"
	"github.com/moutend/go-wca/pkg/wca"
)

type audioSessionNotification struct {
	vtbl *audioSessionNotificationVtbl
	ref  int32
	cb   func(session *wca.IAudioSessionControl) error
}

type audioSessionNotificationVtbl struct {
	QueryInterface   uintptr
	AddRef           uintptr
	Release          uintptr
	OnSessionCreated uintptr
}

func newAudioSessionNotification(cb func(session *wca.IAudioSessionControl) error) *wca.IAudioSessionNotification {
	// Create the VTable
	vtbl := &audioSessionNotificationVtbl{
		QueryInterface:   syscall.NewCallback(asn_QueryInterface),
		AddRef:           syscall.NewCallback(asn_AddRef),
		Release:          syscall.NewCallback(asn_Release),
		OnSessionCreated: syscall.NewCallback(asn_OnSessionCreated),
	}

	// Create the object
	asn := &audioSessionNotification{
		vtbl: vtbl,
		ref:  1,
		cb:   cb,
	}

	// Cast to wca.IAudioSessionNotification
	// Note: wca.IAudioSessionNotification has a VTable of type *wca.IAudioSessionEventsVtbl (bug in go-wca v0.3.0)
	// We cast our *audioSessionNotificationVtbl to *wca.IAudioSessionEventsVtbl via unsafe.Pointer
	// This is safe because we only expose the 4 methods we implemented, and Windows will only call those
	// based on the IID we report in QueryInterface.
	return (*wca.IAudioSessionNotification)(unsafe.Pointer(asn))
}

func asn_QueryInterface(this *audioSessionNotification, iid *ole.GUID, punk **ole.IUnknown) uintptr {
	if ole.IsEqualGUID(iid, ole.IID_IUnknown) || ole.IsEqualGUID(iid, wca.IID_IAudioSessionNotification) {
		asn_AddRef(this)
		*punk = (*ole.IUnknown)(unsafe.Pointer(this))
		return ole.S_OK
	}
	*punk = nil
	return ole.E_NOINTERFACE
}

func asn_AddRef(this *audioSessionNotification) uintptr {
	this.ref++
	return uintptr(this.ref)
}

func asn_Release(this *audioSessionNotification) uintptr {
	this.ref--
	return uintptr(this.ref)
}

func asn_OnSessionCreated(this *audioSessionNotification, newSession *wca.IAudioSessionControl) uintptr {
	if this.cb != nil {
		if err := this.cb(newSession); err != nil {
			// Log error? We don't have logger here easily.
			// Maybe we should just return S_OK as we can't propagate error to Windows RPC
		}
	}
	return ole.S_OK
}
