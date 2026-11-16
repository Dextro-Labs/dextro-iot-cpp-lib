#include <dextro-iot/atomic_file.hpp>

#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

namespace dextro::iot {

bool write_file_atomic(const std::string& path, std::string_view content, mode_t mode) noexcept
{
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        return false;
    }

    std::size_t done = 0;
    while (done < content.size()) {
        const ssize_t put = ::write(fd, content.data() + done, content.size() - done);
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(put);
    }

    (void)::fchmod(fd, mode);
    // `fsync` antes do `rename`: sem ele o rename pode chegar ao disco antes
    // dos dados, e a queda de energia deixa o nome novo apontando para nada.
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    ::close(fd);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    // E o diretório: sem ele o `rename` pode não estar no disco na queda de
    // energia — e o /data da bancada é ext4 SEM journal, onde isso já
    // corrompeu arquivo. Falha aqui não desfaz nada: o arquivo novo já está no
    // lugar, só a durabilidade do nome fica por conta do kernel.
    const std::size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string(".")
                            : slash == 0               ? std::string("/")
                                                       : path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool read_file(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

}  // namespace dextro::iot
