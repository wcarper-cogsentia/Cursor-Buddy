from buddy.project import project_label_from_path


def test_scot_platform():
    assert project_label_from_path("/Users/walt/SCOT-platform") == "SCOT"


def test_plain_name():
    assert project_label_from_path("/tmp/Aiden") == "Aiden"
