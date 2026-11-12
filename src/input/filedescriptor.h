#pragma once

#include <unistd.h>

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor = -1) : m_fd(descriptor) {}

    ~FileDescriptor() {
        reset();
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const {
        return m_fd;
    }

    int release() {
        const int descriptor = m_fd;
        m_fd = -1;
        return descriptor;
    }

    void reset(int descriptor = -1) {
        if (m_fd >= 0) {
            close(m_fd);
        }
        m_fd = descriptor;
    }

private:
    int m_fd;
};
