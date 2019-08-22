// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "cyan_store.h"

#include <boost/algorithm/string/trim.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include "common/safe_io.h"
#include "os/Transaction.h"

#include "crimson/common/buffer_io.h"
#include "crimson/common/config_proxy.h"
#include "crimson/os/cyan_collection.h"
#include "crimson/os/cyan_object.h"

namespace {
  seastar::logger& logger() {
    return ceph::get_logger(ceph_subsys_filestore);
  }
}

using ceph::common::local_conf;

namespace ceph::os {

using ObjectRef = boost::intrusive_ptr<Object>;

CyanStore::CyanStore(const std::string& path)
  : path{path}
{}

CyanStore::~CyanStore() = default;

seastar::future<> CyanStore::mount()
{
  ceph::bufferlist bl;
  std::string fn = path + "/collections";
  std::string err;
  if (int r = bl.read_file(fn.c_str(), &err); r < 0) {
    throw std::runtime_error("read_file");
  }

  std::set<coll_t> collections;
  auto p = bl.cbegin();
  ceph::decode(collections, p);

  for (auto& coll : collections) {
    std::string fn = fmt::format("{}/{}", path, coll);
    ceph::bufferlist cbl;
    if (int r = cbl.read_file(fn.c_str(), &err); r < 0) {
      throw std::runtime_error("read_file");
    }
    CollectionRef c{new Collection{coll}};
    auto p = cbl.cbegin();
    c->decode(p);
    coll_map[coll] = c;
    used_bytes += c->used_bytes();
  }
  return seastar::now();
}

seastar::future<> CyanStore::umount()
{
  return seastar::do_with(std::set<coll_t>{}, [this](auto& collections) {
    return seastar::do_for_each(coll_map, [&collections, this](auto& coll) {
      auto& [col, ch] = coll;
      collections.insert(col);
      ceph::bufferlist bl;
      ceph_assert(ch);
      ch->encode(bl);
      std::string fn = fmt::format("{}/{}", path, col);
      return ceph::buffer::write_file(std::move(bl), fn);
    }).then([&collections, this] {
      ceph::bufferlist bl;
      ceph::encode(collections, bl);
      std::string fn = fmt::format("{}/collections", path);
      return ceph::buffer::write_file(std::move(bl), fn);
    });
  });
}

seastar::future<> CyanStore::mkfs(uuid_d new_osd_fsid)
{
  return read_meta("fsid").then([=](auto r, auto fsid_str) {
    if (r == -ENOENT) {
      if (new_osd_fsid.is_zero()) {
        osd_fsid.generate_random();
      } else {
        osd_fsid = new_osd_fsid;
      }
      return write_meta("fsid", fmt::format("{}", osd_fsid));
    } else if (r < 0) {
      throw std::runtime_error("read_meta");
    } else {
      logger().info("{} already has fsid {}", __func__, fsid_str);
      if (!osd_fsid.parse(fsid_str.c_str())) {
        throw std::runtime_error("failed to parse fsid");
      } else if (osd_fsid != new_osd_fsid) {
        logger().error("on-disk fsid {} != provided {}", osd_fsid, new_osd_fsid);
        throw std::runtime_error("unmatched osd_fsid");
      } else {
	return seastar::now();
      }
    }
  }).then([this]{
    std::string fn = path + "/collections";
    ceph::bufferlist bl;
    std::set<coll_t> collections;
    ceph::encode(collections, bl);
    return ceph::buffer::write_file(std::move(bl), fn);
  }).then([this] {
    return write_meta("type", "memstore");
  });
}

store_statfs_t CyanStore::stat() const
{
  logger().debug("{}", __func__);
  store_statfs_t st;
  st.total = ceph::common::local_conf().get_val<Option::size_t>("memstore_device_bytes");
  st.available = st.total - used_bytes;
  return st;
}

seastar::future<std::vector<ghobject_t>, ghobject_t>
CyanStore::list_objects(CollectionRef c,
                        const ghobject_t& start,
                        const ghobject_t& end,
                        uint64_t limit) const
{
  logger().debug("{} {} {} {} {}",
                 __func__, c->cid, start, end, limit);
  std::vector<ghobject_t> objects;
  objects.reserve(limit);
  ghobject_t next = ghobject_t::get_max();
  for (const auto& [oid, obj] :
         boost::make_iterator_range(c->object_map.lower_bound(start),
                                    c->object_map.end())) {
    std::ignore = obj;
    if (oid >= end || objects.size() >= limit) {
      next = oid;
      break;
    }
    objects.push_back(oid);
  }
  return seastar::make_ready_future<std::vector<ghobject_t>, ghobject_t>(
    std::move(objects), next);
}

seastar::future<CollectionRef> CyanStore::create_new_collection(const coll_t& cid)
{
  auto c = new Collection{cid};
  new_coll_map[cid] = c;
  return seastar::make_ready_future<CollectionRef>(c);
}

seastar::future<CollectionRef> CyanStore::open_collection(const coll_t& cid)
{
  return seastar::make_ready_future<CollectionRef>(_get_collection(cid));
}

seastar::future<std::vector<coll_t>> CyanStore::list_collections()
{
  std::vector<coll_t> collections;
  for (auto& coll : coll_map) {
    collections.push_back(coll.first);
  }
  return seastar::make_ready_future<std::vector<coll_t>>(std::move(collections));
}

seastar::future<ceph::bufferlist> CyanStore::read(CollectionRef c,
                                            const ghobject_t& oid,
                                            uint64_t offset,
                                            size_t len,
                                            uint32_t op_flags)
{
  logger().debug("{} {} {} {}~{}",
                __func__, c->cid, oid, offset, len);
  if (!c->exists) {
    throw std::runtime_error(fmt::format("collection does not exist: {}", c->cid));
  }
  ObjectRef o = c->get_object(oid);
  if (!o) {
    throw std::runtime_error(fmt::format("object does not exist: {}", oid));
  }
  if (offset >= o->get_size())
    return seastar::make_ready_future<ceph::bufferlist>();
  size_t l = len;
  if (l == 0 && offset == 0)  // note: len == 0 means read the entire object
    l = o->get_size();
  else if (offset + l > o->get_size())
    l = o->get_size() - offset;
  ceph::bufferlist bl;
  if (int r = o->read(offset, l, bl); r < 0) {
    throw std::runtime_error("read");
  }
  return seastar::make_ready_future<ceph::bufferlist>(std::move(bl));
}

seastar::future<ceph::bufferptr> CyanStore::get_attr(CollectionRef c,
                                                     const ghobject_t& oid,
                                                     std::string_view name) const
{
  logger().debug("{} {} {}",
                __func__, c->cid, oid);
  auto o = c->get_object(oid);
  if (!o) {
    return seastar::make_exception_future<ceph::bufferptr>(
      EnoentException(fmt::format("object does not exist: {}", oid)));
  }
  if (auto found = o->xattr.find(name); found != o->xattr.end()) {
    return seastar::make_ready_future<ceph::bufferptr>(found->second);
  } else {
    return seastar::make_exception_future<ceph::bufferptr>(
      EnoentException(fmt::format("attr does not exist: {}/{}", oid, name)));
  }
}

seastar::future<CyanStore::attrs_t> CyanStore::get_attrs(CollectionRef c,
                                                         const ghobject_t& oid)
{
  logger().debug("{} {} {}",
                __func__, c->cid, oid);
  auto o = c->get_object(oid);
  if (!o) {
    throw std::runtime_error(fmt::format("object does not exist: {}", oid));
  }
  return seastar::make_ready_future<attrs_t>(o->xattr);
}

seastar::future<CyanStore::omap_values_t>
CyanStore::omap_get_values(CollectionRef c,
                           const ghobject_t& oid,
                           const omap_keys_t& keys)
{
  logger().debug("{} {} {}",
                __func__, c->cid, oid);
  auto o = c->get_object(oid);
  if (!o) {
    throw std::runtime_error(fmt::format("object does not exist: {}", oid));
  }
  omap_values_t values;
  for (auto& key : keys) {
    if (auto found = o->omap.find(key); found != o->omap.end()) {
      values.insert(*found);
    }
  }
  return seastar::make_ready_future<omap_values_t>(std::move(values));
}

seastar::future<bool, CyanStore::omap_values_t>
CyanStore::omap_get_values(
    CollectionRef c,
    const ghobject_t &oid,
    const std::optional<string> &start
  ) {
  logger().debug(
    "{} {} {}",
    __func__, c->cid, oid);
  auto o = c->get_object(oid);
  if (!o) {
    throw std::runtime_error(fmt::format("object does not exist: {}", oid));
  }
  omap_values_t values;
  for (auto i = start ? o->omap.upper_bound(*start) : o->omap.begin();
       values.size() < MAX_KEYS_PER_OMAP_GET_CALL && i != o->omap.end();
       ++i) {
    values.insert(*i);
  }
  return seastar::make_ready_future<bool, omap_values_t>(
    true, values);
}

seastar::future<> CyanStore::do_transaction(CollectionRef ch,
                                            Transaction&& t)
{
  int r = 0;
  try {
    auto i = t.begin();
    while (i.have_op()) {
      Transaction::Op* op = i.decode_op();
      r = 0;
      switch (op->op) {
      case Transaction::OP_NOP:
	break;
      case Transaction::OP_REMOVE:
      {
	coll_t cid = i.get_cid(op->cid);
	ghobject_t oid = i.get_oid(op->oid);
	r = _remove(cid, oid);
	if (r == -ENOENT) {
	  r = 0;
	}
      }
      break;
      case Transaction::OP_TOUCH:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        r = _touch(cid, oid);
      }
      break;
      case Transaction::OP_WRITE:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        uint64_t off = op->off;
        uint64_t len = op->len;
        uint32_t fadvise_flags = i.get_fadvise_flags();
        ceph::bufferlist bl;
        i.decode_bl(bl);
        r = _write(cid, oid, off, len, bl, fadvise_flags);
      }
      break;
      case Transaction::OP_TRUNCATE:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        uint64_t off = op->off;
        r = _truncate(cid, oid, off);
      }
      break;
      case Transaction::OP_SETATTR:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        std::string name = i.decode_string();
        ceph::bufferlist bl;
        i.decode_bl(bl);
        std::map<std::string, bufferptr> to_set;
        to_set[name] = bufferptr(bl.c_str(), bl.length());
        r = _setattrs(cid, oid, to_set);
      }
      break;
      case Transaction::OP_MKCOLL:
      {
        coll_t cid = i.get_cid(op->cid);
        r = _create_collection(cid, op->split_bits);
      }
      break;
      case Transaction::OP_OMAP_SETKEYS:
      {
        coll_t cid = i.get_cid(op->cid);
        ghobject_t oid = i.get_oid(op->oid);
        std::map<std::string, ceph::bufferlist> aset;
        i.decode_attrset(aset);
        r = _omap_set_values(cid, oid, std::move(aset));
      }
      break;
      case Transaction::OP_OMAP_SETHEADER:
      {
	const coll_t &cid = i.get_cid(op->cid);
	const ghobject_t &oid = i.get_oid(op->oid);
	ceph::bufferlist bl;
	i.decode_bl(bl);
	r = _omap_set_header(cid, oid, bl);
      }
      break;
      case Transaction::OP_OMAP_RMKEYS:
      {
	const coll_t &cid = i.get_cid(op->cid);
	const ghobject_t &oid = i.get_oid(op->oid);
	omap_keys_t keys;
	i.decode_keyset(keys);
	r = _omap_rmkeys(cid, oid, keys);
      }
      break;
      case Transaction::OP_OMAP_RMKEYRANGE:
      {
	const coll_t &cid = i.get_cid(op->cid);
	const ghobject_t &oid = i.get_oid(op->oid);
	string first, last;
	first = i.decode_string();
	last = i.decode_string();
	r = _omap_rmkeyrange(cid, oid, first, last);
      }
      break;
      case Transaction::OP_COLL_HINT:
      {
        ceph::bufferlist hint;
        i.decode_bl(hint);
	// ignored
	break;
      }
      default:
	logger().error("bad op {}", static_cast<unsigned>(op->op));
	abort();
      }
      if (r < 0) {
	break;
      }
    }
  } catch (std::exception &e) {
    logger().error("{} got exception {}", __func__, e);
    r = -EINVAL;
  }
  if (r < 0) {
    logger().error(" transaction dump:\n");
    JSONFormatter f(true);
    f.open_object_section("transaction");
    t.dump(&f);
    f.close_section();
    std::stringstream str;
    f.flush(str);
    logger().error("{}", str.str());
    ceph_assert(r == 0);
  }
  for (auto i : {
      t.get_on_applied(),
      t.get_on_commit(),
      t.get_on_applied_sync()}) {
    if (i) {
      i->complete(0);
    }
  }
  return seastar::now();
}

int CyanStore::_remove(const coll_t& cid, const ghobject_t& oid)
{
  logger().debug("{} cid={} oid={}",
                __func__, cid, oid);
  auto c = _get_collection(cid);
  if (!c)
    return -ENOENT;

  auto i = c->object_hash.find(oid);
  if (i == c->object_hash.end())
    return -ENOENT;
  used_bytes -= i->second->get_size();
  c->object_hash.erase(i);
  c->object_map.erase(oid);
  return 0;
}

int CyanStore::_touch(const coll_t& cid, const ghobject_t& oid)
{
  logger().debug("{} cid={} oid={}",
                __func__, cid, oid);
  auto c = _get_collection(cid);
  if (!c)
    return -ENOENT;

  c->get_or_create_object(oid);
  return 0;
}

int CyanStore::_write(const coll_t& cid, const ghobject_t& oid,
                      uint64_t offset, size_t len, const ceph::bufferlist& bl,
                      uint32_t fadvise_flags)
{
  logger().debug("{} {} {} {} ~ {}",
                __func__, cid, oid, offset, len);
  assert(len == bl.length());

  auto c = _get_collection(cid);
  if (!c)
    return -ENOENT;

  ObjectRef o = c->get_or_create_object(oid);
  if (len > 0 && !local_conf()->memstore_debug_omit_block_device_write) {
    const ssize_t old_size = o->get_size();
    o->write(offset, bl);
    used_bytes += (o->get_size() - old_size);
  }

  return 0;
}

int CyanStore::_omap_set_values(
  const coll_t& cid,
  const ghobject_t& oid,
  std::map<std::string, ceph::bufferlist> &&aset)
{
  logger().debug(
    "{} {} {} {} keys",
    __func__, cid, oid, aset.size());

  auto c = _get_collection(cid);
  if (!c)
    return -ENOENT;

  ObjectRef o = c->get_or_create_object(oid);
  for (auto &&i: aset) {
    o->omap.insert(std::move(i));
  }
  return 0;
}

int CyanStore::_omap_set_header(
  const coll_t& cid,
  const ghobject_t& oid,
  const ceph::bufferlist &header)
{
  logger().debug(
    "{} {} {} {} bytes",
    __func__, cid, oid, header.length());

  auto c = _get_collection(cid);
  if (!c)
    return -ENOENT;

  ObjectRef o = c->get_or_create_object(oid);
  o->omap_header = header;
  return 0;
}

int CyanStore::_omap_rmkeys(
  const coll_t& cid,
  const ghobject_t& oid,
  const omap_keys_t& aset)
{
  logger().debug(
    "{} {} {} {} keys",
    __func__, cid, oid, aset.size());

  auto c = _get_collection(cid);
  if (!c)
    return -ENOENT;

  ObjectRef o = c->get_or_create_object(oid);
  for (auto &i: aset) {
    o->omap.erase(i);
  }
  return 0;
}

int CyanStore::_omap_rmkeyrange(
  const coll_t& cid,
  const ghobject_t& oid,
  const std::string &first,
  const std::string &last)
{
  logger().debug(
    "{} {} {} first={} last={}",
    __func__, cid, oid, first, last);

  auto c = _get_collection(cid);
  if (!c)
    return -ENOENT;

  ObjectRef o = c->get_or_create_object(oid);
  for (auto i = o->omap.lower_bound(first);
       i != o->omap.end() && i->first <= last;
       o->omap.erase(i++));
  return 0;
}

int CyanStore::_truncate(const coll_t& cid, const ghobject_t& oid, uint64_t size)
{
  logger().debug("{} cid={} oid={} size={}",
                __func__, cid, oid, size);
  auto c = _get_collection(cid);
  if (!c)
    return -ENOENT;

  ObjectRef o = c->get_object(oid);
  if (!o)
    return -ENOENT;
  if (local_conf()->memstore_debug_omit_block_device_write)
    return 0;
  const ssize_t old_size = o->get_size();
  int r = o->truncate(size);
  used_bytes += (o->get_size() - old_size);
  return r;
}

int CyanStore::_setattrs(const coll_t& cid, const ghobject_t& oid,
                         std::map<std::string,bufferptr>& aset)
{
  logger().debug("{} cid={} oid={}",
                __func__, cid, oid);
  auto c = _get_collection(cid);
  if (!c)
    return -ENOENT;

  ObjectRef o = c->get_object(oid);
  if (!o)
    return -ENOENT;
  for (std::map<std::string, bufferptr>::const_iterator p = aset.begin();
       p != aset.end(); ++p)
    o->xattr[p->first] = p->second;
  return 0;
}

int CyanStore::_create_collection(const coll_t& cid, int bits)
{
  auto result = coll_map.insert(std::make_pair(cid, CollectionRef()));
  if (!result.second)
    return -EEXIST;
  auto p = new_coll_map.find(cid);
  assert(p != new_coll_map.end());
  result.first->second = p->second;
  result.first->second->bits = bits;
  new_coll_map.erase(p);
  return 0;
}

CollectionRef CyanStore::_get_collection(const coll_t& cid)
{
  auto cp = coll_map.find(cid);
  if (cp == coll_map.end())
    return {};
  return cp->second;
}

seastar::future<> CyanStore::write_meta(const std::string& key,
					const std::string& value)
{
  std::string v = value;
  v += "\n";
  if (int r = safe_write_file(path.c_str(), key.c_str(),
                              v.c_str(), v.length());
      r < 0) {
    throw std::runtime_error{fmt::format("unable to write_meta({})", key)};
  }
  return seastar::make_ready_future<>();
}

seastar::future<int, std::string> CyanStore::read_meta(const std::string& key)
{
  std::string fsid(4096, '\0');
  int r = safe_read_file(path.c_str(), key.c_str(), fsid.data(), fsid.size());
  if (r > 0) {
    fsid.resize(r);
    // drop trailing newlines
    boost::algorithm::trim_right_if(fsid,
				    [](unsigned char c) {return isspace(c);});
  } else {
    fsid.clear();
  }
  return seastar::make_ready_future<int, std::string>(r, fsid);
}

uuid_d CyanStore::get_fsid() const
{
  return osd_fsid;
}

unsigned CyanStore::get_max_attr_name_length() const
{
  // arbitrary limitation exactly like in the case of MemStore.
  return 256;
}
}
