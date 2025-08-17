# usage: ./make-a-release.py [previous_version]
# create a new release tag and update CHANGELOG.md. If previous-version is not
# provided, it will be inferred from the latest git tag.

# XXX: I set out to use pygit2, but turns out certain things I need aren't available there,
#      so this script uses git command line also. ¯\_(ツ)_/¯
import pygit2
import datetime
import sys
import tempfile
import subprocess
import re
from pathlib import Path

repo = pygit2.Repository('.')

status = repo.status(untracked_files='no')
for filepath, flags in status.items():
	if flags != pygit2.enums.FileStatus.CURRENT:
		print(f"Filepath {filepath} isn't clean")
		print("Repository is dirty, please commit or stash your changes")
		sys.exit(1)

all_tags = {tag.peel(pygit2.Commit).id: tag.shorthand for tag in repo.references.iterator(pygit2.enums.ReferenceFilter.TAGS)}
previous_version = sys.argv[1] if len(sys.argv) > 1 else None
merge_base = None

# now, since stable releases are branched off from next, and some commits in next might have already been backported/cherry-picked to stable,
# so we need to go through unique commits in the previous stable release, and filter out the ones that were already released.

if previous_version is None:
	queue = [repo.head.peel(pygit2.Commit)]
	h = 0
	while h < len(queue) and queue[h].id not in all_tags:
		queue.extend(queue[h].parents)
		h += 1
	print(f'Previous release is {all_tags[queue[h].id]}, commit {queue[h].id}')
	previous_version = merge_base = queue[h].id
else:
	try:
		previous_version = repo.references[f'refs/tags/{previous_version}'].peel(pygit2.Commit).id
	except KeyError:
		print(f'Tag {previous_version} does not exist')
		sys.exit(1)
	merge_base = repo.merge_base(repo.head.peel(pygit2.Commit).id, previous_version)
	print(f'Merge base between HEAD and {sys.argv[1]} is {merge_base}')

# collect commits:
#   - from merge_base to HEAD
#   - from merge_base to previous_version
our_commits = set(subprocess.run(['git', 'log', '--format=%H', f'{merge_base}..HEAD'], check=True, capture_output=True) \
                        .stdout.decode().strip().split('\n'))
if merge_base != previous_version:
	their_commits = subprocess.run(['git', 'log', '--format=%H', f'{merge_base}..{previous_version}'], check=True, capture_output=True) \
		                      .stdout.decode().strip().split('\n')
else:
	their_commits = []

for commit in their_commits:
	if commit in our_commits:
		our_commits.remove(commit)
	commit = repo.get(commit)
	lines = commit.message.split('\n')
	r = re.compile(r'(?:cherry picked|backported) from commit ([0-9a-f]+)')
	for line in lines:
		m = r.search(line)
		if m:
			if m.group(1) not in our_commits:
				print(f'WARN: Cherry pick {m.group(1)} not found in our commits')
			else:
				print(f'Cherry pick: {m.group(1)}')
			our_commits.remove(m.group(1))

our_commits = [repo.get(commit) for commit in our_commits]
our_commits.sort(key=lambda x: x.commit_time)

changelog_categories = {
	'BugFix': 'Bug fixes',
	'BuildFix': 'Build fixes',
	'BuildChange': 'Build changes',
	'NewFeature': 'New features',
	'Deprecation': 'Deprecations',
	'Internal': 'Internal changes',
	'Uncategorized': 'Other changes',
}

changelogs = {category: [] for category in changelog_categories}

for commit in our_commits:
	lines = commit.message.split('\n')
	related_issues = []
	r_related = re.compile(r'(?:Fixes|Related|Related-to):?\s+(.+)')
	for line in lines:
		m = r_related.fullmatch(line)
		if m:
			for issue in m.group(1).split(' '):
				related_issues.append(int(issue[1:]))
		elif line.startswith('Fixes') or line.startswith('Related'):
			print(f'WARN: Possibly invalid issue reference: {line}')
		elif line.startswith('Merge pull request #'):
			related_issues.append(int(line.split()[3][1:]))

	changelog = [i for i, line in enumerate(lines) if line.startswith('Changelog:')]
	if len(changelog) > 1:
		print('WARN: Multiple Changelog lines')
	if not changelog:
		continue
	line = changelog[0] + 1
	changelog = lines[changelog[0]].removeprefix('Changelog:')
	while line < len(lines) and lines[line].strip() != '':
		changelog += ' ' + lines[line]
		line += 1
	cat, changelog = changelog.split(':', 1)
	cat = cat.strip()
	changelog = changelog.strip()
	if cat not in changelog_categories:
		print(f'WARN: Unknown category: {cat}')
		changelog = f'({cat}) {changelog}'
		cat = 'Uncategorized'
	if related_issues:
		changelog += f' ({" ".join(f"#{issue}" for issue in related_issues)})'

	changelogs[cat].append(changelog)
	print(f'Commit {commit.id}: {changelog} [{cat}]')

# are we already on a tag?
for ref in repo.references.iterator(pygit2.enums.ReferenceFilter.TAGS):
	if ref.shorthand.startswith('v') and ref.peel(pygit2.Commit) == repo.head.peel(pygit2.Commit):
		print(f'HEAD is already a release tag: {ref.shorthand}')
		sys.exit(1)

with tempfile.TemporaryDirectory() as tempdir:
	# can this commit be built?
	try:
		subprocess.run(['meson', 'setup', tempdir], check=True)
		subprocess.run(['ninja', '-C', tempdir], check=True)
	except subprocess.CalledProcessError:
		print('This commit cannot be built')
		sys.exit(1)

	version = subprocess.run([Path(tempdir) / 'src' / 'picom', '--version'], capture_output=True, check=True).stdout.decode().strip()
	version = version.split(' ')[0]

	if not version.startswith('v'):
		print('Version should start with "v"')
		sys.exit(1)

print(f'Version to be released is {version}')
try:
	existing_tag = repo.references[f'refs/tags/{version}']
	print(f'Tag {version} already exists')
	sys.exit(1)
except KeyError:
	pass

today = datetime.date.today()
changelog = f'# {version} ({today.strftime("%Y-%b-%d")})\n'
for category, changes in changelogs.items():
	if changes:
		changelog += f'\n## {changelog_categories[category]}\n\n'
		for change in changes:
			changelog += f'* {change}\n'
changelog += '\n'

with open('CHANGELOG.md', 'r') as f:
	old = f.read()

with open('CHANGELOG.md', 'w') as f:
	f.write(changelog + old)

subprocess.run(['git', 'add', 'CHANGELOG.md'], check=True)
subprocess.run(['git', 'commit', '-s', '-m', f'release: {version}'], check=True)
subprocess.run(['git', 'tag', '-a', '-s', '-m', version, version], check=True)
