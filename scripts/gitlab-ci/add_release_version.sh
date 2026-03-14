#!/bin/sh
sed -i "s/#define APP_GIT_TAG \".*\"/#define APP_GIT_TAG \"$CI_COMMIT_TAG\"/" project/app/src/utilities/app_git_tag.h
VERSION_TAG=$(echo $CI_COMMIT_TAG | sed 's/^v//')
VERSION_MAJOR=$(echo $VERSION_TAG | cut -d. -f1)
VERSION_MINOR=$(echo $VERSION_TAG | cut -d. -f2)
PATCHLEVEL=$(echo $VERSION_TAG | cut -d. -f3 | cut -d- -f1)
echo "VERSION_MAJOR = $VERSION_MAJOR" > project/app/VERSION
echo "VERSION_MINOR = $VERSION_MINOR" >> project/app/VERSION
echo "PATCHLEVEL = $PATCHLEVEL" >> project/app/VERSION
echo "VERSION_TWEAK = 0" >> project/app/VERSION
echo "EXTRAVERSION = 0" >> project/app/VERSION
echo "" >> project/app/VERSION