package notessync

import "testing"

// validRepo is the guard that keeps req.Repo from traversing the api.github.com
// URL path (e.g. "../../other"), so cover the traversal and malformed cases.
func TestValidRepo(t *testing.T) {
	valid := []string{
		"octocat/Hello-World",
		"my.org/my.repo",
		"a_b/c-d",
		"user123/repo.git",
	}
	for _, r := range valid {
		if !validRepo(r) {
			t.Errorf("validRepo(%q) = false, want true", r)
		}
	}

	invalid := []string{
		"",
		"noslash",
		"owner/",
		"/repo",
		"owner/re/po",    // extra segment
		"../../x",        // traversal
		"a/..",           // dot-dot segment
		"../b",           // dot-dot segment
		".",              // no slash + dot
		"owner/repo\x00", // NUL
		"own er/repo",    // space
		"owner/re?po",    // query char
	}
	for _, r := range invalid {
		if validRepo(r) {
			t.Errorf("validRepo(%q) = true, want false", r)
		}
	}
}

// safeName is applied to every file name in the sync path so a name like
// "../secrets.txt" can't write outside notes/.
func TestSafeNameRejectsTraversal(t *testing.T) {
	ok := []string{"20250101_120000.txt", "note.md", "a-b_c.txt"}
	for _, n := range ok {
		if _, valid := safeName(n); !valid {
			t.Errorf("safeName(%q) rejected, want accepted", n)
		}
	}

	bad := []string{
		"",
		"../secrets.txt",
		"a/b",
		"..",
		".",
		".hidden",
		"x\x00y",
	}
	for _, n := range bad {
		if _, valid := safeName(n); valid {
			t.Errorf("safeName(%q) accepted, want rejected", n)
		}
	}
}
