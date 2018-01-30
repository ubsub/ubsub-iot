#!/usr/bin/env python
import subprocess
import json
import re

class Version:
  def __init__(self, s):
    self.__nums = map(lambda x: int(x), s.split('.'))

  def bumpPatch(self):
    self.__nums[2] += 1

  def bumpMinor(self):
    self.__nums[1] += 1

  def bumpMajor(self):
    self.__nums[0] += 1

  def __str__(self):
    return str.join('.', map(lambda x: str(x), self.__nums))

  def __repr__(self):
    return str(self)

def sh(s):
  subprocess.call(s, shell=True)

# Get and bump version
library = json.loads(open('library.json', 'r').read())
version = Version(library['version'])
print "Current version: %s" % version
version.bumpPatch()
print "New version: %s" % version

# Rewrite json
library['version'] = str(version)
open('library.json', 'w').write(json.dumps(library, sort_keys=True, indent=2, separators=(',', ': ')))

# Rewrite library.properties
libprop = open('library.properties', 'r').read()
libprop = re.sub(r'version=([0-9.]+)', 'version=' + str(version), libprop)
open('library.properties', 'w').write(libprop)

# Make a signed commit, tag, push
sh("git add library.json library.properties")
sh("git commit -s -m 'Bump version to %s'" % version)
sh("git tag -as -m %s %s" % (version, version))
sh("git push")
sh("git push --tag")

print("Uploading to particle.io...")
sh("particle library upload")
sh("particle library publish")


