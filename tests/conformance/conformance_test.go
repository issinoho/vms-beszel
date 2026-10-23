// Package vmsconformance checks vms-beszel's encodings against upstream
// beszel's own structs and decoding code (DECISIONS.md D6).
//
// tools/conformance.sh copies the pinned upstream tree to out/conformance,
// drops this file into internal/vmsconformance (Go only lets a module import
// its own internal packages) and runs it with the fixtures written by
// build/gen_fixtures.  The pinned tree in cache/ is never modified.
package vmsconformance

import (
	"bytes"
	"encoding/hex"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"

	"github.com/fxamacker/cbor/v2"
	"github.com/henrygd/beszel/internal/common"
	"github.com/henrygd/beszel/internal/entities/monitor"
	"github.com/henrygd/beszel/internal/entities/smart"
	"github.com/henrygd/beszel/internal/entities/system"
	"github.com/henrygd/beszel/internal/entities/zfs"
	"github.com/henrygd/beszel/internal/hub/transport"
)

// strict rejects anything upstream's structs don't declare, and duplicate keys.
var strict cbor.DecMode

func init() {
	var err error
	strict, err = cbor.DecOptions{
		ExtraReturnErrors: cbor.ExtraDecErrorUnknownField,
		DupMapKey:         cbor.DupMapKeyEnforcedAPF,
	}.DecMode()
	if err != nil {
		panic(err)
	}
}

func fixture(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(os.Getenv("BZ_FIXTURES"), name+".cbor"))
	if err != nil {
		t.Fatalf("fixture %s: %v", name, err)
	}
	return b
}

// sameBytes re-encodes v with upstream's encoder and requires our exact bytes.
func sameBytes(t *testing.T, name string, ours []byte, v any) {
	t.Helper()
	theirs, err := cbor.Marshal(v)
	if err != nil {
		t.Fatalf("%s: upstream marshal: %v", name, err)
	}
	if !bytes.Equal(ours, theirs) {
		t.Errorf("%s: bytes differ from upstream's encoding\n ours   %s\n theirs %s", name,
			hex.EncodeToString(ours), hex.EncodeToString(theirs))
	}
}

// sameValue compares generically decoded trees (for maps Go would reorder).
func sameValue(t *testing.T, name string, ours []byte, v any) {
	t.Helper()
	theirs, err := cbor.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	var a, b any
	if err := cbor.Unmarshal(ours, &a); err != nil {
		t.Fatal(err)
	}
	if err := cbor.Unmarshal(theirs, &b); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(a, b) {
		t.Errorf("%s: value differs from upstream's re-encoding", name)
	}
}

func decodeCombined(t *testing.T, name string) (system.CombinedData, []byte) {
	t.Helper()
	b := fixture(t, name)
	var cd system.CombinedData
	if err := strict.Unmarshal(b, &cd); err != nil {
		t.Fatalf("%s: strict decode into system.CombinedData: %v", name, err)
	}
	return cd, b
}

func TestCombinedData(t *testing.T) {
	for _, name := range []string{"combined-1", "combined-details-1", "combined-empty-1"} {
		cd, b := decodeCombined(t, name)
		sameBytes(t, name, b, cd)
	}
	cd, b := decodeCombined(t, "combined-many")
	sameValue(t, "combined-many", b, cd)

	cd, _ = decodeCombined(t, "combined-details-1")
	s, i, d := cd.Stats, cd.Info, cd.Details
	checks := []struct {
		what string
		ok   bool
	}{
		{"Stats.Cpu", s.Cpu == 16.45},
		{"Stats.CpuBreakdown", reflect.DeepEqual(s.CpuBreakdown, []float64{10.2, 6.25, 0, 0, 83.55})},
		{"Stats.CpuCoresUsage", reflect.DeepEqual([]uint8(s.CpuCoresUsage), []uint8{19, 14})},
		{"Stats.LoadAvg", s.LoadAvg == [3]float64{0.52, 0.31, 0.2}},
		{"Stats.Mem", s.Mem == 7.09 && s.MemUsed == 1.03 && s.MemPct == 14.57 && s.MemBuffCache == 0.17},
		{"Stats.Swap", s.Swap == 1.78 && s.SwapUsed == 0.31},
		{"Stats.MemBytes", s.MemBytes == [4]uint64{7612895232, 1110341632, 182452224, 0}},
		{"Stats.SwapBytes", s.SwapBytes == [2]uint64{1911029760, 332398592}},
		{"Stats.ExtraFs deprecated rates absent", s.ExtraFs["Tools"] != nil && s.ExtraFs["Tools"].DiskReadPs == 0 && s.ExtraFs["Tools"].DiskWritePs == 0},
		{"Stats.Disk", s.DiskTotal == 21.46 && s.DiskUsed == 11.92 && s.DiskPct == 55.57},
		{"Stats.Bandwidth", s.Bandwidth == [2]uint64{5120, 81920}},
		{"Stats.NetworkInterfaces", s.NetworkInterfaces["EIA0"] == [4]uint64{5120, 81920, 82151203, 88139130}},
		{"Stats.ExtraFs", s.ExtraFs["Tools"] != nil && s.ExtraFs["Tools"].DiskTotal == 0.94 && s.ExtraFs["Tools"].DiskUsed == 0.41},
		{"Stats.ExtraFs internal fields unset", s.ExtraFs["Tools"] != nil && !s.ExtraFs["Tools"].Root && s.ExtraFs["Tools"].Name == ""},
		{"Info.Threads", i.Threads == 2},
		{"Info.Uptime", i.Uptime == 28020},
		{"Info.AgentVersion", i.AgentVersion == "0.21.1"},
		{"Info.BandwidthBytes", i.BandwidthBytes == 5120+81920},
		{"Info.ConnectionType", i.ConnectionType == system.ConnectionTypeWebSocket},
		{"Info.ExtraFsPct", i.ExtraFsPct["Tools"] == 43.62},
		{"Info.RootDiskName", i.RootDiskName == "System"},
		{"Info deprecated fields empty", i.Hostname == "" && i.CpuModel == "" && i.KernelVersion == "" && i.Os == 0},
		{"Details present", d != nil},
		{"Details.Os is Linux", d != nil && d.Os == system.Linux},
		{"Details.OsName", d != nil && d.OsName == "OpenVMS E9.2-4 (x86-64)"},
		{"Details.MemoryTotal", d != nil && d.MemoryTotal == 7620722688},
		{"Containers null", cd.Containers == nil},
		{"Monitors present and empty", cd.Monitors != nil && len(cd.Monitors) == 0},
	}
	for _, c := range checks {
		if !c.ok {
			t.Errorf("combined-details-1: %s", c.what)
		}
	}
}

// response decodes an envelope the way the hub does (default options), and
// strictly; then runs the hub's own transport.UnmarshalResponse.
func response(t *testing.T, name string, action common.WebSocketAction, dest any) (common.AgentResponse, error) {
	t.Helper()
	b := fixture(t, name)
	var lenient, strictResp common.AgentResponse
	if err := cbor.Unmarshal(b, &lenient); err != nil {
		t.Fatalf("%s: hub decode of AgentResponse: %v", name, err)
	}
	if err := strict.Unmarshal(b, &strictResp); err != nil {
		t.Fatalf("%s: strict decode of AgentResponse: %v", name, err)
	}
	if lenient.Id == nil || *lenient.Id != 4242 {
		t.Errorf("%s: Id lost", name)
	}
	if strings.HasSuffix(name, "-1") {
		sameBytes(t, name, b, lenient)
	}
	return lenient, transport.UnmarshalResponse(lenient, action, dest)
}

func TestReplies(t *testing.T) {
	var cd system.CombinedData
	if _, err := response(t, "reply-getdata-1", common.GetData, &cd); err != nil || cd.Details == nil || cd.Stats.Cpu != 16.45 {
		t.Errorf("GetData: %v", err)
	}

	var fp common.FingerprintResponse
	if _, err := response(t, "reply-fingerprint-1", common.CheckFingerprint, &fp); err != nil ||
		fp.Fingerprint != "9a1dfdd2a3a88e9a701aaa18461b7ab871342721caf26c6f" || fp.Hostname != "" {
		t.Errorf("CheckFingerprint: %v %+v", err, fp)
	}
	fp = common.FingerprintResponse{}
	if _, err := response(t, "reply-fingerprint-sysinfo-1", common.CheckFingerprint, &fp); err != nil ||
		fp.Hostname != "x86vms" || fp.Port != "45876" || fp.Name != "VMS x86" {
		t.Errorf("CheckFingerprint with sys info: %v %+v", err, fp)
	}

	// SMART: upstream without smartctl sends {0:id}; the hub logs this error at Info (PROTOCOL.md 3.3)
	var sm smart.SmartDataResponse
	if _, err := response(t, "reply-smart-1", common.GetSmartData, &sm); err == nil ||
		err.Error() != "no SMART data in response" {
		t.Errorf("GetSmartData: want the bare-host error, got %v", err)
	}

	var z zfs.ZfsData
	if _, err := response(t, "reply-zfs-1", common.GetZfsData, &z); err != nil || !z.Complete || len(z.Pools) != 0 {
		t.Errorf("GetZfsData: %v %+v", err, z)
	}
	// Q5: upstream's own encoding of the same value
	sameBytes(t, "zfs payload", fixture(t, "reply-zfs-1")[6:], zfs.ZfsData{Complete: true}) // after {0: 4242, 7:

	var sr monitor.SyncResponse
	if _, err := response(t, "reply-sync-1", common.SyncNetworkMonitors, &sr); err != nil ||
		!reflect.DeepEqual(sr, monitor.SyncResponse{}) {
		t.Errorf("SyncNetworkMonitors: %v %+v", err, sr)
	}

	for _, n := range []struct {
		name   string
		action common.WebSocketAction
	}{{"reply-containerlogs-1", common.GetContainerLogs}, {"reply-containerinfo-1", common.GetContainerInfo}} {
		var s string
		if _, err := response(t, n.name, n.action, &s); err != nil || s != "" {
			t.Errorf("%s: %v", n.name, err)
		}
	}

	var pu system.PackageUpdates
	if _, err := response(t, "reply-packages-1", common.GetPackageUpdates, &pu); err != nil {
		t.Errorf("GetPackageUpdates: %v", err)
	}

	for _, name := range []string{"reply-systemdinfo-1", "reply-systemdlogs-1", "reply-unknown-1"} {
		b := fixture(t, name)
		var r common.AgentResponse
		if err := strict.Unmarshal(b, &r); err != nil || r.Error == "" {
			t.Errorf("%s: want an Error reply: %v", name, err)
		}
	}
}

// TestUpstreamOmissions documents the library behaviour our encoder copies.
func TestUpstreamOmissions(t *testing.T) {
	b, _ := cbor.Marshal(system.FsStats{DiskTotal: 1, Root: true, Name: "x", Mountpoint: "/"})
	var m map[any]any
	_ = cbor.Unmarshal(b, &m)
	keys := []string{}
	for k := range m {
		keys = append(keys, strings.TrimSpace(strings.ToLower(hex.EncodeToString([]byte{byte(k.(uint64))}))))
	}
	sort.Strings(keys)
	if !reflect.DeepEqual(keys, []string{"00", "01"}) {
		t.Errorf("FsStats encodes keys %v; we send 0, 1", keys)
	}

	// An idle sample: no memory or swap bytes, so keys 41 and 42 are left out
	b, _ = cbor.Marshal(system.Stats{})
	m = map[any]any{}
	_ = cbor.Unmarshal(b, &m)
	for _, k := range []uint64{41, 42} {
		if _, ok := m[k]; ok {
			t.Errorf("Stats{} encodes key %d; we leave it out when zero", k)
		}
	}
}
