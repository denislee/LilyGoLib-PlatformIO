package weather

import "testing"

func TestIPAPIStatusOK(t *testing.T) {
	cases := []struct {
		name string
		body string
		ok   bool
	}{
		{"success", `{"status":"success","city":"Kyiv","lat":50.4}`, true},
		{"fail", `{"status":"fail","message":"private range"}`, false},
		{"missing status", `{"city":"Kyiv"}`, false},
		{"garbage", `not json at all`, false},
		{"empty", ``, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := ipAPIStatusOK([]byte(tc.body))
			if tc.ok && err != nil {
				t.Fatalf("want accept, got error: %v", err)
			}
			if !tc.ok && err == nil {
				t.Fatal("want reject, got nil error (would be cached as success)")
			}
		})
	}
}
