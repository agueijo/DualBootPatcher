/*
 * Copyright (C) 2014-2018  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of DualBootPatcher
 *
 * DualBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DualBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DualBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mbutil/path.h"

#include <chrono>
#include <vector>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <libgen.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mbcommon/error_code.h"


namespace mb::util
{

oc::result<std::string> get_cwd()
{
    std::unique_ptr<char, decltype(free) *> cwd(getcwd(nullptr, 0), &free);
    if (!cwd) {
        return ec_from_errno();
    }

    return cwd.get();
}

std::string dir_name(std::string path)
{
    return dirname(path.data());
}

std::string base_name(std::string path)
{
    return basename(path.data());
}

oc::result<std::string> read_link(const std::string &path)
{
    std::vector<char> buf;
    ssize_t len;

    buf.resize(64);

    for (;;) {
        len = readlink(path.c_str(), buf.data(), buf.size() - 1);
        if (len < 0) {
            return ec_from_errno();
        } else if (static_cast<size_t>(len) == buf.size() - 1) {
            buf.resize(buf.size() << 1);
        } else {
            break;
        }
    }

    buf[static_cast<size_t>(len)] = '\0';
    return buf.data();
}

/*!
 * \brief Split a path into pieces
 *
 * This function splits the path into pieces with the directory separator '/'
 * being the delimiter. If the path is absolute (ie. begins with '/'), then the
 * first piece will be empty. If a piece is equal to the current directory '.',
 * then it is simply removed. If multiple directory separator characters are
 * located next to one another (ie. 'a/////b'), they are treated as one
 * directory separator (ie. becomes 'a/b').
 *
 * \param path Path to split
 *
 * \return Split pieces of the path
 */
std::vector<std::string> path_split(std::string path)
{
    char *p;
    char *save_ptr;

    std::vector<std::string> split;

    // For absolute paths
    if (!path.empty() && path[0] == '/') {
        split.push_back(std::string());
    }

    p = strtok_r(path.data(), "/", &save_ptr);
    while (p != nullptr) {
        // Ignore useless references to '.' in the path
        if (strcmp(p, ".") != 0) {
            split.push_back(p);
        }
        p = strtok_r(nullptr, "/", &save_ptr);
    }

    return split;
}

/*!
 * \brief Join path pieces into a path
 *
 * This function simply joins each piece with the directory separator character.
 * If the first piece is empty, then the path is treated as an absolute path and
 * a directory separator character will be placed at the beginning of the
 * resulting path. If the pieces contain only a single empty piece, then the
 * root directory '/' is returned.
 *
 * \param components Path pieces to join
 *
 * \return Joined path
 */
std::string path_join(const std::vector<std::string> &components)
{
    std::string path;
    for (auto it = components.begin(); it != components.end(); ++it) {
        if (it->empty()) {
            path += "/";
        } else {
            path += *it;
            if (it != components.end() - 1) {
                path += "/";
            }
        }
    }
    return path;
}

/*!
 * \brief Normalize path to remove '..' pieces
 *
 * This function will remove a '..' piece if any of the following conditions are
 * met:
 * - If the previous piece is the root directory '/', only the '..' piece is
 *   removed as '..' is meaning less for the root directory (ie. '/' == '/..')
 * - If the previous piece is not '..', then remove both the previous
 *   piece and the '..' (eg. 'a/b/..' -> 'a')
 *
 * \note This function will not treat '.' pieces specially as they should have
 *       been stripped out by path_split(). If the path pieces are manually
 *       created, take care to not add '.'. Otherwise, the result will be
 *       incorrect. For example, '/usr/bin/./..' will become '/usr/bin'.
 *
 * \param components Reference to list of path pieces
 */
void normalize_path(std::vector<std::string> &components)
{
    std::vector<std::string>::iterator prev_it;

    for (auto it = components.begin(); it != components.end();) {
        if (it != components.begin()) {
            prev_it = it - 1;

            // After the removal, it will point to the piece after '..'
            if (*it == ".." && *prev_it != "..") {
                // If previous directory is the root directory, then '..' is a
                // noop, so just remove the '..'. Otherwise, remove the previous
                // piece and the '..' piece.
                if (prev_it->empty()) {
                    it = components.erase(it);
                } else {
                    it = components.erase(prev_it);
                    it = components.erase(it);
                }
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }
}

/*!
 * \brief Get the relative path from a starting directory
 *
 * This function will get the relative path of \a path starting from \a start.
 * Both \a path and \a start will be normalized before calculating the relative
 * path. That way, paths containing '..' will be handled correctly. For example,
 * calling relative_path("/usr/bin", "/usr/include/glib-2.0/..") will return
 * "../bin" as expected.
 *
 * If, in the directory tree, \a start is at a higher level than the parent
 * directory of \a path, then the function will return false and set \a errno to
 * \a EINVAL. This is because there is no way of determining the intermediate
 * paths to form the relative path between the two directories. For example, if
 * we want to determine the relative path of "a/b" starting from "..", the
 * result would be "[some_dir]/a/b", but there is no way to determine what
 * \a some_dir is.
 *
 * \note This function does not traverse the filesystem at all. It works purely
 *       on the given path strings.
 *
 * \return The relative path if it was successfully computed.
 *         std::errc::invalid_argument if:
 *         - \a path is absolute and \a start is relative or vice versa
 *         - \a path or \a start is empty
 *         - an intermediate path could not be computed
 */
oc::result<std::string> relative_path(const std::string &path,
                                      const std::string &start)
{
    if (path.empty() || start.empty()
            || (path[0] == '/' && start[0] != '/')
            || (path[0] != '/' && start[0] == '/')) {
        return std::errc::invalid_argument;
    }

    std::vector<std::string> path_pieces(path_split(path));
    std::vector<std::string> start_pieces(path_split(start));
    std::vector<std::string> result_pieces;

    normalize_path(path_pieces);
    normalize_path(start_pieces);

    // Find the number of common path segments
    size_t common;
    for (common = 0;
            common < path_pieces.size() && common < start_pieces.size()
            && path_pieces[common] == start_pieces[common];
            ++common);

    // Add '..' for the remaining path segments in 'start'
    for (size_t i = common; i < start_pieces.size(); ++i) {
        if (start_pieces[i] == "..") {
            return std::errc::invalid_argument;
        }
        result_pieces.push_back("..");
    }

    // Add remaining path segments in 'path'
    for (size_t i = common; i < path_pieces.size(); ++i) {
        result_pieces.push_back(path_pieces[i]);
    }

    return path_join(result_pieces);
}

/*!
 * \brief An strcmp() for paths
 *
 * This function will normalize \a path1 and \a path2 before they are passed to
 * std::string::compare(). This way, extra slashes, '.', '..', etc. are handled
 * before the string comparison is performed.
 *
 * \note This function does not traverse the filesystem at all. It works purely
 *       on the given path strings.
 *
 * \return An integer less than, equal to, or greater than zero if \a path1 is
 *         found, respectively, to be less than, equal to, or greater than
 *         \a path2 lexicographically.
 */
int path_compare(const std::string &path1, const std::string &path2)
{
    if (path1.empty() || path2.empty()) {
        return false;
    }

    std::vector<std::string> path1_pieces(path_split(path1));
    std::vector<std::string> path2_pieces(path_split(path2));

    normalize_path(path1_pieces);
    normalize_path(path2_pieces);

    return path_join(path1_pieces).compare(path_join(path2_pieces));
}

bool wait_for_path(const std::string &path, std::chrono::milliseconds timeout)
{
    using namespace std::chrono;

    auto until = steady_clock::now() + timeout;
    struct stat sb;
    int ret;

    while ((ret = stat(path.c_str(), &sb)) < 0 && steady_clock::now() < until) {
        usleep(10000);
    }

    return ret == 0;
}

bool path_exists(const std::string &path, bool follow_symlinks)
{
    struct stat sb;
    return (follow_symlinks ? stat : lstat)(path.c_str(), &sb) == 0;
}

}
