#!/usr/bin/env python3
import os
import subprocess
import shutil


def find_components_with_docs():
    """
    Search the repo for component folders that have docs/book.toml
    """
    # Get the repository root directory (works both locally and in GitHub Actions)
    repo_root = os.getcwd()
    components = []

    for item in os.listdir(repo_root):
        component_path = os.path.join(repo_root, item)
        docs_path = os.path.join(component_path, "docs")
        book_toml_path = os.path.join(docs_path, "book.toml")

        if (
            os.path.isdir(component_path)
            and os.path.isdir(docs_path)
            and os.path.isfile(book_toml_path)
        ):
            components.append(item)
            print(f"Found component with docs: {item}")

    return components


def build_component_docs(component_name):
    """
    Build documentation for a component using mdbook
    """
    repo_root = os.getcwd()
    component_docs_path = os.path.join(repo_root, component_name, "docs")

    print(f"Building docs for {component_name}...")
    try:
        subprocess.run(["mdbook", "build", component_docs_path], check=True)
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error building docs for {component_name}: {e}")
        return False


def copy_docs_to_public(component_name):
    """
    Copy the built documentation to the public directory
    """
    repo_root = os.getcwd()
    source_path = os.path.join(repo_root, component_name, "docs", "book")
    dest_path = os.path.join(repo_root, "public", component_name)

    if os.path.exists(source_path):
        # Remove destination if it exists to avoid issues with copytree
        if os.path.exists(dest_path):
            shutil.rmtree(dest_path)

        # Create parent directory if needed
        os.makedirs(os.path.dirname(dest_path), exist_ok=True)

        # Copy the documentation
        shutil.copytree(source_path, dest_path)
        print(f"Copied docs from {source_path} to {dest_path}")
    else:
        print(f"Source path {source_path} does not exist, skipping copy")


def main():
    # Create public directory
    repo_root = os.getcwd()
    public_path = os.path.join(repo_root, "public")

    # Clean public directory
    if os.path.exists(public_path):
        shutil.rmtree(public_path)
    os.mkdir(public_path)

    # Find components with docs
    components = find_components_with_docs()
    print(
        f"Found {len(components)} components with documentation: {', '.join(components)}"
    )

    # Build docs for each component and copy to public directory
    for component in components:
        if build_component_docs(component):
            copy_docs_to_public(component)


if __name__ == "__main__":
    main()
