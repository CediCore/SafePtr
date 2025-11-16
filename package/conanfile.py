from conan import ConanFile
from conan.tools.files import copy
import os

class SafePtrConan(ConanFile):
    name = "safeptr"
    version = "1.0.0"
    license = "MIT"
    url = "https://github.com/CediCore/SafePtr"
    description = "Thread-safe smart pointer with lock-free reads"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "include/*", "LICENSE", "README.md"

    def package(self):
        copy(self, "safeptr.hpp", src="include", dst=os.path.join(self.package_folder, "include"))
        copy(self, "LICENSE", src=".", dst=os.path.join(self.package_folder, "licenses"))
        copy(self, "README.md", src=".", dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.includedirs = ["include"]
